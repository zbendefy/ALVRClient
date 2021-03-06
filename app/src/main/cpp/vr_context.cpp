#include <unistd.h>
#include <jni.h>
#include <VrApi.h>
#include <VrApi_Types.h>
#include <VrApi_Helpers.h>
#include <VrApi_SystemUtils.h>
#include <VrApi_Input.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <string>
#include <map>
#include <vector>
#include "utils.h"
#include "packet_types.h"
#include "render.h"
#include "vr_context.h"


void VrContext::onVrModeChange() {
    if (Resumed && window != NULL) {
        if (Ovr == NULL) {
            LOGI("Entering VR mode.");
            ovrModeParms parms = vrapi_DefaultModeParms(&java);

            parms.Flags |= VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN;

            parms.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW;
            parms.Display = (size_t) egl.Display;
            parms.WindowSurface = (size_t) window;
            parms.ShareContext = (size_t) egl.Context;

            Ovr = vrapi_EnterVrMode(&parms);

            if (Ovr == NULL) {
                LOGE("Invalid ANativeWindow");
                return;
            }

            int CpuLevel = 3;
            int GpuLevel = 3;
            vrapi_SetClockLevels(Ovr, CpuLevel, GpuLevel);
            vrapi_SetPerfThread(Ovr, VRAPI_PERF_THREAD_TYPE_MAIN, gettid());
        }
    } else {
        if (Ovr != NULL) {
            LOGI("Leaving VR mode.");
            vrapi_LeaveVrMode(Ovr);

            LOGI("Leaved VR mode.");
            Ovr = NULL;
        }
    }
}

void VrContext::PushBlackFinal() {
    int frameFlags = 0;
    frameFlags |= VRAPI_FRAME_FLAG_FLUSH | VRAPI_FRAME_FLAG_FINAL;

    ovrLayerProjection2 layer = vrapi_DefaultLayerBlackProjection2();
    layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;

    const ovrLayerHeader2 * layers[] =
            {
                    &layer.Header
            };

    ovrSubmitFrameDescription2 frameDesc = {};
    frameDesc.Flags = frameFlags;
    frameDesc.SwapInterval = 1;
    frameDesc.FrameIndex = FrameIndex;
    frameDesc.DisplayTime = GetTimeInSeconds();
    frameDesc.LayerCount = 1;
    frameDesc.Layers = layers;

    vrapi_SubmitFrame2(Ovr, &frameDesc);
}

void VrContext::BackButtonAction() {
    if ( BackButtonState == BACK_BUTTON_STATE_PENDING_SHORT_PRESS && !BackButtonDown )
    {
        if ( ( GetTimeInSeconds() - BackButtonDownStartTime ) >
             vrapi_GetSystemPropertyFloat( &java, VRAPI_SYS_PROP_BACK_BUTTON_SHORTPRESS_TIME ) )
        {
            LOG("Detect back button short press.");
            PushBlackFinal();
            vrapi_ShowSystemUI( &java, VRAPI_SYS_UI_CONFIRM_QUIT_MENU );
            BackButtonState = BACK_BUTTON_STATE_NONE;
        }
    }
}


void VrContext::chooseRefreshRate() {
    int numberOfRefreshRates = vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_NUM_SUPPORTED_DISPLAY_REFRESH_RATES);
    std::vector<float> refreshRates(numberOfRefreshRates);
    vrapi_GetSystemPropertyFloatArray(&java, VRAPI_SYS_PROP_SUPPORTED_DISPLAY_REFRESH_RATES, &refreshRates[0], numberOfRefreshRates);

    support72hz = false;
    std::string refreshRateList = "";
    char str[100];
    for(int i = 0; i < numberOfRefreshRates; i++) {
        snprintf(str, sizeof(str), "%f", refreshRates[i]);
        refreshRateList += str;
        if(i != numberOfRefreshRates - 1) {
            refreshRateList += ", ";
        }
        if(((int)refreshRates[i]) == 72) {
            support72hz = true;
        }
    }
    LOG("Supported refresh rates: %s", refreshRateList.c_str());

    if(support72hz) {
        LOG("Use 72 Hz refresh rate.");
        vrapi_SetDisplayRefreshRate(Ovr, 72.0f);
    }else {
        LOG("Use 60 Hz refresh rate.");
    }
}

void VrContext::initialize(JNIEnv *env, jobject activity, bool ARMode) {
    LOG("Initializing EGL.");
    this->env = env;
    java.Env = env;
    env->GetJavaVM(&java.Vm);
    java.ActivityObject = env->NewGlobalRef(activity);

    eglInit();

    bool multi_view;
    EglInitExtensions(&multi_view);

    const ovrInitParms initParms = vrapi_DefaultInitParms(&java);
    int32_t initResult = vrapi_Initialize(&initParms);
    if (initResult != VRAPI_INITIALIZE_SUCCESS) {
        // If initialization failed, vrapi_* function calls will not be available.
        LOGE("vrapi_Initialize failed");
        return;
    }

    UseMultiview = (multi_view && vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_MULTIVIEW_AVAILABLE));
    LOG("UseMultiview:%d", UseMultiview);

    GLint textureUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnits);
    LOGI("GL_VENDOR=%s", glGetString(GL_VENDOR));
    LOGI("GL_RENDERER=%s", glGetString(GL_RENDERER));
    LOGI("GL_VERSION=%s", glGetString(GL_VERSION));
    LOGI("GL_MAX_TEXTURE_IMAGE_UNITS=%d", textureUnits);

    chooseRefreshRate();

    //
    // Generate texture for SurfaceTexture which is output of MediaCodec.
    //
    m_ARMode = ARMode;

    int textureCount = 2;
    if(m_ARMode) {
        textureCount++;
    }
    GLuint textures[textureCount];
    glGenTextures(textureCount, textures);

    SurfaceTextureID = textures[0];
    loadingTexture = textures[1];

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, SurfaceTextureID);

    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);

    if(m_ARMode) {
        CameraTexture = textures[2];
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, CameraTexture);

        glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
                        GL_NEAREST);
        glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
                        GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
    }

    FrameBufferWidth = vrapi_GetSystemPropertyInt(&java,
                                                  VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
    FrameBufferHeight = vrapi_GetSystemPropertyInt(&java,
                                                   VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
    ovrRenderer_Create(&Renderer, &java, UseMultiview, FrameBufferWidth, FrameBufferHeight
            , SurfaceTextureID, loadingTexture, CameraTexture, m_ARMode);
    ovrRenderer_CreateScene(&Renderer);

    BackButtonState = BACK_BUTTON_STATE_NONE;
    BackButtonDown = false;
    BackButtonDownStartTime = 0.0;

    position_offset_y = 0.0;
}


void VrContext::destroy() {
    LOG("Destroying EGL.");

    ovrRenderer_Destroy(&Renderer);

    GLuint textures[2] = {SurfaceTextureID, loadingTexture};
    glDeleteTextures(2, textures);
    if (m_ARMode){
        glDeleteTextures(1, &CameraTexture);
    }

    eglDestroy();

    vrapi_Shutdown();
}


void VrContext::setControllerInfo(TrackingInfo *packet, double displayTime) {
    ovrInputCapabilityHeader curCaps;
    ovrResult result;

    for (uint32_t deviceIndex = 0; vrapi_EnumerateInputDevices(Ovr, deviceIndex, &curCaps) >= 0; deviceIndex++) {
        //LOG("Device %d: Type=%d ID=%d", deviceIndex, curCaps.Type, curCaps.DeviceID);
        if (curCaps.Type == ovrControllerType_TrackedRemote) {
            // Gear VR / Oculus Go 3DoF Controller
            ovrInputTrackedRemoteCapabilities remoteCapabilities;
            remoteCapabilities.Header = curCaps;
            result = vrapi_GetInputDeviceCapabilities(Ovr, &remoteCapabilities.Header);
            if (result != ovrSuccess) {
                continue;
            }
            packet->flags |= TrackingInfo::FLAG_CONTROLLER_ENABLE;

            if ((remoteCapabilities.ControllerCapabilities & ovrControllerCaps_LeftHand) != 0) {
                packet->flags |= TrackingInfo::FLAG_CONTROLLER_LEFTHAND;
            }
            if ((remoteCapabilities.ControllerCapabilities & ovrControllerCaps_ModelOculusGo) != 0) {
                packet->flags |= TrackingInfo::FLAG_CONTROLLER_OCULUSGO;
            }

            ovrInputStateTrackedRemote remoteInputState;
            remoteInputState.Header.ControllerType = remoteCapabilities.Header.Type;

            result = vrapi_GetCurrentInputState(Ovr, remoteCapabilities.Header.DeviceID, &remoteInputState.Header);
            if (result != ovrSuccess) {
                continue;
            }
            packet->controllerButtons = remoteInputState.Buttons;
            if (remoteInputState.TrackpadStatus) {
                packet->flags |= TrackingInfo::FLAG_CONTROLLER_TRACKPAD_TOUCH;
            }
            // Normalize to -1.0 - +1.0 for OpenVR Input. y-asix should be reversed.
            packet->controllerTrackpadPosition.x = remoteInputState.TrackpadPosition.x / remoteCapabilities.TrackpadMaxX * 2.0f - 1.0f;
            packet->controllerTrackpadPosition.y = remoteInputState.TrackpadPosition.y / remoteCapabilities.TrackpadMaxY * 2.0f - 1.0f;
            packet->controllerTrackpadPosition.y = -packet->controllerTrackpadPosition.y;
            packet->controllerBatteryPercentRemaining = remoteInputState.BatteryPercentRemaining;
            packet->controllerRecenterCount = remoteInputState.RecenterCount;

            ovrTracking tracking;
            if (vrapi_GetInputTrackingState(Ovr, remoteCapabilities.Header.DeviceID,
                                            displayTime, &tracking) != ovrSuccess) {
                LOG("vrapi_GetInputTrackingState failed. Device was disconnected?");
            } else {
                memcpy(&packet->controller_Pose_Orientation,
                       &tracking.HeadPose.Pose.Orientation,
                       sizeof(tracking.HeadPose.Pose.Orientation));
                memcpy(&packet->controller_Pose_Position,
                       &tracking.HeadPose.Pose.Position,
                       sizeof(tracking.HeadPose.Pose.Position));
                memcpy(&packet->controller_AngularVelocity,
                       &tracking.HeadPose.AngularVelocity,
                       sizeof(tracking.HeadPose.AngularVelocity));
                memcpy(&packet->controller_LinearVelocity,
                       &tracking.HeadPose.LinearVelocity,
                       sizeof(tracking.HeadPose.LinearVelocity));
                memcpy(&packet->controller_AngularAcceleration,
                       &tracking.HeadPose.AngularAcceleration,
                       sizeof(tracking.HeadPose.AngularAcceleration));
                memcpy(&packet->controller_LinearAcceleration,
                       &tracking.HeadPose.LinearAcceleration,
                       sizeof(tracking.HeadPose.LinearAcceleration));
            }
        }else if(curCaps.Type == ovrControllerType_Headset) {
            // Gear VR Headset
            ovrInputHeadsetCapabilities capabilities;
            capabilities.Header = curCaps;
            ovrResult result = vrapi_GetInputDeviceCapabilities(Ovr, &capabilities.Header);
            if(result == ovrSuccess) {
                LOG("Device(Headset) %d: Type=%d ID=%d Cap=%08X Buttons=%08X Max=%d,%d Size=%f,%f", deviceIndex, curCaps.Type, curCaps.DeviceID
                , capabilities.ControllerCapabilities, capabilities.ButtonCapabilities
                , capabilities.TrackpadMaxX, capabilities.TrackpadMaxY
                , capabilities.TrackpadSizeX, capabilities.TrackpadSizeY);

                ovrInputStateHeadset remoteInputState;
                remoteInputState.Header.ControllerType = capabilities.Header.Type;
                ovrResult result = vrapi_GetCurrentInputState(Ovr,
                                                              capabilities.Header.DeviceID,
                                                              &remoteInputState.Header);

                if (result == ovrSuccess) {
                    float normalized_x = remoteInputState.TrackpadPosition.x / capabilities.TrackpadMaxX;
                    float normalized_y = remoteInputState.TrackpadPosition.y / capabilities.TrackpadMaxY;
                    LOG("Headset trackpad: status=%d %f, %f (%f, %f) (%08X)", remoteInputState.TrackpadStatus
                    , remoteInputState.TrackpadPosition.x
                    , remoteInputState.TrackpadPosition.y
                    , normalized_x, normalized_y
                    , remoteInputState.Buttons);

                    // Change overlay mode and height on AR mode by the trackpad of headset.
                    if(m_ARMode) {
                        if (previousHeadsetTrackpad && remoteInputState.TrackpadStatus != 0) {
                            position_offset_y += (normalized_y - previousHeadsetY) * 2.0f;
                            LOG("Changing position_offset_y: %f", position_offset_y);
                        }

                        if (previousHeadsetTrackpad && remoteInputState.TrackpadStatus == 0) {
                            if (normalized_x < 0.4) {
                                if (g_AROverlayMode > 0) {
                                    g_AROverlayMode--;
                                } else {
                                    g_AROverlayMode = 3;
                                }
                                LOG("Changing AROverlayMode. New=%d", g_AROverlayMode);
                            }
                            if (normalized_x > 0.6) {
                                if (g_AROverlayMode < 3) {
                                    g_AROverlayMode++;
                                } else {
                                    g_AROverlayMode = 0;
                                }
                                LOG("Changing AROverlayMode. New=%d", g_AROverlayMode);
                            }
                        }
                    }

                    previousHeadsetTrackpad = remoteInputState.TrackpadStatus != 0;
                    previousHeadsetY = normalized_y;
                }
            }
        }
    }
}

// Called TrackingThread. So, we can't use this->env.
void VrContext::sendTrackingInfo(JNIEnv *env_, jobject callback, double displayTime, ovrTracking2 *tracking
        , const ovrVector3f *other_tracking_position, const ovrQuatf *other_tracking_orientation) {
    jbyteArray array = env_->NewByteArray(sizeof(TrackingInfo));
    TrackingInfo *packet = (TrackingInfo *) env_->GetByteArrayElements(array, 0);
    memset(packet, 0, sizeof(TrackingInfo));

    uint64_t clientTime = getTimestampUs();

    packet->type = ALVR_PACKET_TYPE_TRACKING_INFO;
    packet->flags = 0;
    packet->clientTime = clientTime;
    packet->FrameIndex = FrameIndex;
    packet->predictedDisplayTime = displayTime;

    memcpy(&packet->HeadPose_Pose_Orientation, &tracking->HeadPose.Pose.Orientation, sizeof(ovrQuatf));
    memcpy(&packet->HeadPose_Pose_Position, &tracking->HeadPose.Pose.Position, sizeof(ovrVector3f));

    if(other_tracking_position && other_tracking_orientation) {
        packet->flags |= TrackingInfo::FLAG_OTHER_TRACKING_SOURCE;
        memcpy(&packet->Other_Tracking_Source_Position, other_tracking_position, sizeof(ovrVector3f));
        memcpy(&packet->Other_Tracking_Source_Orientation, other_tracking_orientation, sizeof(ovrQuatf));
    }

    setControllerInfo(packet, displayTime);

    env_->ReleaseByteArrayElements(array, (jbyte *) packet, 0);

    FrameLog(FrameIndex, "Sending tracking info.");

    jclass clazz = env_->GetObjectClass(callback);
    jmethodID sendTracking = env_->GetMethodID(clazz, "onSendTracking", "([BIJ)V");

    env_->CallVoidMethod(callback, sendTracking, array, sizeof(TrackingInfo), FrameIndex);
}

// Called TrackingThread. So, we can't use this->env.
int64_t VrContext::fetchTrackingInfo(JNIEnv *env_, jobject callback, jfloatArray position_, jfloatArray orientation_){
    std::shared_ptr<TrackingFrame> frame(new TrackingFrame());

    FrameIndex++;

    frame->frameIndex = FrameIndex;
    frame->fetchTime = getTimestampUs();

    frame->displayTime = vrapi_GetPredictedDisplayTime(Ovr, FrameIndex);
    frame->tracking = vrapi_GetPredictedTracking2(Ovr, frame->displayTime);

    {
        MutexLock lock(trackingFrameMutex);
        trackingFrameMap.insert(std::pair<uint64_t, std::shared_ptr<TrackingFrame> >(FrameIndex, frame));
        if (trackingFrameMap.size() > MAXIMUM_TRACKING_FRAMES) {
            trackingFrameMap.erase(trackingFrameMap.cbegin());
        }
    }

    if(position_ != NULL) {
        // AR mode
        ovrVector3f position;
        ovrQuatf orientation;

        jfloat *position_c = env_->GetFloatArrayElements(position_, NULL);
        memcpy(&position, position_c, sizeof(float) * 3);
        env_->ReleaseFloatArrayElements(position_, position_c, 0);

        jfloat *orientation_c = env_->GetFloatArrayElements(orientation_, NULL);
        memcpy(&orientation, orientation_c, sizeof(float) * 4);
        env_->ReleaseFloatArrayElements(orientation_, orientation_c, 0);

        // Rotate PI/2 around (0, 0, -1)
        // Orientation provided by ARCore is portrait mode orientation.
        ovrQuatf quat;
        quat.x = 0;
        quat.y = 0;
        quat.z = -sqrtf(0.5);
        quat.w = sqrtf(0.5);
        ovrQuatf orientation_rotated = quatMultipy(&orientation, &quat);

        position.y += position_offset_y;

        sendTrackingInfo(env_, callback, frame->displayTime, &frame->tracking, &position, &orientation_rotated);
    } else {
        // Non AR
        sendTrackingInfo(env_, callback, frame->displayTime, &frame->tracking, NULL, NULL);
    }

    return FrameIndex;
}


void VrContext::onChangeSettings(int EnableTestMode, int Suspend){
    enableTestMode = EnableTestMode;
    suspend = Suspend;
}

void VrContext::onSurfaceCreated(jobject surface){
    LOG("onSurfaceCreated called. Resumed=%d Window=%p Ovr=%p", Resumed, window, Ovr);
    window = ANativeWindow_fromSurface(env, surface);

    onVrModeChange();
}

void VrContext::onSurfaceDestroyed(){
    LOG("onSurfaceDestroyed called. Resumed=%d Window=%p Ovr=%p", Resumed, window, Ovr);
    if (window != NULL) {
        ANativeWindow_release(window);
    }
    window = NULL;

    onVrModeChange();
}

void VrContext::onSurfaceChanged(jobject surface){
    LOG("onSurfaceChanged called. Resumed=%d Window=%p Ovr=%p", Resumed, window, Ovr);
    ANativeWindow *newWindow = ANativeWindow_fromSurface(env, surface);
    if (newWindow != window) {
        LOG("Replacing ANativeWindow. %p != %p", newWindow, window);
        ANativeWindow_release(window);
        window = NULL;
        onVrModeChange();

        window = newWindow;
        if (window != NULL) {
            onVrModeChange();
        }
    } else if (newWindow != NULL) {
        LOG("Got same ANativeWindow. %p == %p", newWindow, window);
        ANativeWindow_release(newWindow);
    }
}
void VrContext::onResume(){
    LOG("onResume called. Resumed=%d Window=%p Ovr=%p", Resumed, window, Ovr);
    Resumed = true;
    onVrModeChange();
}
void VrContext::onPause(){
    LOG("onPause called. Resumed=%d Window=%p Ovr=%p", Resumed, window, Ovr);
    Resumed = false;
    onVrModeChange();
}

bool VrContext::onKeyEvent(int keyCode, int action){
    LOG("HandleKeyEvent: keyCode=%d action=%d", keyCode, action);
    // Handle back button.
    if ( keyCode == AKEYCODE_BACK )
    {
        if ( action == AKEY_EVENT_ACTION_DOWN )
        {
            if ( !BackButtonDown )
            {
                BackButtonDownStartTime = GetTimeInSeconds();
            }
            BackButtonDown = true;
        }
        else if ( action == AKEY_EVENT_ACTION_UP )
        {
            if ( BackButtonState == BACK_BUTTON_STATE_NONE )
            {
                if ( ( GetTimeInSeconds() - BackButtonDownStartTime ) <
                     vrapi_GetSystemPropertyFloat( &java, VRAPI_SYS_PROP_BACK_BUTTON_SHORTPRESS_TIME ) )
                {
                    BackButtonState = BACK_BUTTON_STATE_PENDING_SHORT_PRESS;
                }
            }
            else if ( BackButtonState == BACK_BUTTON_STATE_SKIP_UP )
            {
                BackButtonState = BACK_BUTTON_STATE_NONE;
            }
            BackButtonDown = false;
        }
        return true;
    }
    return false;
}


void VrContext::render(jobject callback, jobject latencyCollector){
    BackButtonAction();

    double currentTime = GetTimeInSeconds();

    unsigned long long completionFence = 0;

    jclass clazz = env->GetObjectClass(callback);
    jmethodID waitFrame = env->GetMethodID(clazz, "waitFrame", "()J");
    int64_t renderedFrameIndex = 0;
    for (int i = 0; i < 10; i++) {
        renderedFrameIndex = env->CallLongMethod(callback, waitFrame);
        if (renderedFrameIndex == -1) {
            // Activity has Paused or connection becomes idle
            return;
        }
        FrameLog(renderedFrameIndex, "Got frame for render. wanted FrameIndex=%lu waiting=%.3f ms delay=%lu",
                 WantedFrameIndex,
                 (GetTimeInSeconds() - currentTime) * 1000,
                 WantedFrameIndex - renderedFrameIndex);
        if (WantedFrameIndex <= renderedFrameIndex) {
            break;
        }
        if ((enableTestMode & 4) != 0) {
            break;
        }
    }

    uint64_t mostOldFrame = 0;
    uint64_t mostRecentFrame = 0;
    std::shared_ptr<TrackingFrame> frame;
    {
        MutexLock lock(trackingFrameMutex);

        if(trackingFrameMap.size() > 0) {
            mostOldFrame = trackingFrameMap.cbegin()->second->frameIndex;
            mostRecentFrame = trackingFrameMap.crbegin()->second->frameIndex;
        }

        const auto it = trackingFrameMap.find(renderedFrameIndex);
        if(it != trackingFrameMap.end()) {
            frame = it->second;
        }else {
            // No matching tracking info. Too old frame.
            LOG("Too old frame has arrived. Instead, we use most old tracking data in trackingFrameMap."
                        "FrameIndex=%lu WantedFrameIndex=%lu trackingFrameMap=(%lu - %lu)",
                renderedFrameIndex, WantedFrameIndex, mostOldFrame, mostRecentFrame);
            if(trackingFrameMap.size() > 0) {
                frame = trackingFrameMap.cbegin()->second;
            } else {
                return;
            }
        }
    }

    FrameLog(renderedFrameIndex, "Frame latency is %lu us. foundFrameIndex=%lu LatestFrameIndex=%lu", getTimestampUs() - frame->fetchTime,
             frame->frameIndex, FrameIndex);

// Render eye images and setup the primary layer using ovrTracking2.
    const ovrLayerProjection2 worldLayer = ovrRenderer_RenderFrame(&Renderer, &java,
                                                                   &frame->tracking,
                                                                   Ovr, &completionFence, false,
                                                                   enableTestMode, g_AROverlayMode);

    clazz = env->GetObjectClass(latencyCollector);
    jmethodID SubmitMethodID = env->GetMethodID(clazz, "Rendered2", "(J)V");
    env->CallVoidMethod(latencyCollector, SubmitMethodID, renderedFrameIndex);

    const ovrLayerHeader2 *layers2[] =
            {
                    &worldLayer.Header
            };

    // TODO
    double DisplayTime = 0.0;

    int SwapInterval = 1;

    ovrSubmitFrameDescription2 frameDesc = {};
    frameDesc.Flags = 0;
    frameDesc.SwapInterval = SwapInterval;
    frameDesc.FrameIndex = renderedFrameIndex;
    frameDesc.CompletionFence = completionFence;
    frameDesc.DisplayTime = DisplayTime;
    frameDesc.LayerCount = 1;
    frameDesc.Layers = layers2;

    WantedFrameIndex = renderedFrameIndex + 1;

    ovrResult res = vrapi_SubmitFrame2(Ovr, &frameDesc);

    FrameLog(renderedFrameIndex, "vrapi_SubmitFrame2 Orientation=(%f, %f, %f, %f)"
            , frame->tracking.HeadPose.Pose.Orientation.x
            , frame->tracking.HeadPose.Pose.Orientation.y
            , frame->tracking.HeadPose.Pose.Orientation.z
            , frame->tracking.HeadPose.Pose.Orientation.w
    );
    if (suspend) {
        LOG("submit enter suspend");
        while (suspend) {
            usleep(1000 * 10);
        }
        LOG("submit leave suspend");
    }
}


void VrContext::renderLoading() {
    double DisplayTime = GetTimeInSeconds();

    BackButtonAction();

    // Show a loading icon.
    FrameIndex++;

    double displayTime = vrapi_GetPredictedDisplayTime(Ovr, FrameIndex);
    ovrTracking2 tracking = vrapi_GetPredictedTracking2(Ovr, displayTime);

    unsigned long long completionFence = 0;

    const ovrLayerProjection2 worldLayer = ovrRenderer_RenderFrame(&Renderer, &java,
                                                                   &tracking,
                                                                   Ovr, &completionFence, true,
                                                                   enableTestMode, g_AROverlayMode);

    const ovrLayerHeader2 *layers[] =
            {
                    &worldLayer.Header
            };


    ovrSubmitFrameDescription2 frameDesc = {};
    frameDesc.Flags = 0;
    frameDesc.SwapInterval = 1;
    frameDesc.FrameIndex = FrameIndex;
    frameDesc.DisplayTime = DisplayTime;
    frameDesc.LayerCount = 1;
    frameDesc.Layers = layers;

    vrapi_SubmitFrame2(Ovr, &frameDesc);
}

void VrContext::setFrameGeometry(int width, int height){
    int eye_width = width / 2;
    if(eye_width != FrameBufferWidth || height != FrameBufferHeight) {
        LOG("Changing FrameBuffer geometry. Old=%dx%d New=%dx%d", FrameBufferWidth, FrameBufferHeight, eye_width, height);
        FrameBufferWidth = eye_width;
        FrameBufferHeight = height;
        ovrRenderer_Destroy(&Renderer);
        ovrRenderer_Create(&Renderer, &java, UseMultiview, FrameBufferWidth, FrameBufferHeight
        , SurfaceTextureID, loadingTexture, CameraTexture, m_ARMode);
        ovrRenderer_CreateScene(&Renderer);
    }
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_polygraphene_alvr_VrContext_initializeNative(JNIEnv *env, jobject instance, jobject activity, bool ARMode) {
    VrContext *context = new VrContext();
    context->initialize(env, activity, ARMode);
    return (jlong)context;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_destroyNative(JNIEnv *env, jobject instance, jlong handle) {
    ((VrContext *)handle)->destroy();
}


extern "C"
JNIEXPORT jint JNICALL
Java_com_polygraphene_alvr_VrContext_getLoadingTextureNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->getLoadingTexture();
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_polygraphene_alvr_VrContext_getSurfaceTextureIDNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->getSurfaceTextureID();
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_polygraphene_alvr_VrContext_getCameraTextureNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->getCameraTexture();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_renderNative(JNIEnv *env, jobject instance, jlong handle, jobject callback, jobject latencyCollector) {
    return ((VrContext *)handle)->render(callback, latencyCollector);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_renderLoadingNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->renderLoading();
}

// Called from TrackingThread
extern "C"
JNIEXPORT jlong JNICALL
Java_com_polygraphene_alvr_VrContext_fetchTrackingInfoNative(JNIEnv *env, jobject instance, jlong handle,
                                                   jobject callback, jfloatArray position_, jfloatArray orientation_) {
    return ((VrContext *)handle)->fetchTrackingInfo(env, callback, position_, orientation_);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onChangeSettingsNative(JNIEnv *env, jobject instance, jlong handle,
                                                  jint EnableTestMode, jint Suspend) {
    ((VrContext *)handle)->onChangeSettings(EnableTestMode, Suspend);
}

//
// Life cycle management.
//

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onSurfaceCreatedNative(JNIEnv *env, jobject instance, jlong handle,
                                                  jobject surface) {
    ((VrContext *)handle)->onSurfaceCreated(surface);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onSurfaceDestroyedNative(JNIEnv *env, jobject instance, jlong handle) {
    ((VrContext *)handle)->onSurfaceDestroyed();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onSurfaceChangedNative(JNIEnv *env, jobject instance, jlong handle, jobject surface) {
    ((VrContext *)handle)->onSurfaceChanged(surface);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onResumeNative(JNIEnv *env, jobject instance, jlong handle) {
    ((VrContext *)handle)->onResume();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_onPauseNative(JNIEnv *env, jobject instance, jlong handle) {
    ((VrContext *)handle)->onPause();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_polygraphene_alvr_VrContext_isVrModeNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->isVrMode();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_polygraphene_alvr_VrContext_is72HzNative(JNIEnv *env, jobject instance, jlong handle) {
    return ((VrContext *)handle)->is72Hz();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_polygraphene_alvr_VrContext_setFrameGeometryNative(JNIEnv *env, jobject instance, jlong handle, jint width,
                                                  jint height) {
    ((VrContext *)handle)->setFrameGeometry(width, height);
}

extern "C"
JNIEXPORT bool JNICALL
Java_com_polygraphene_alvr_VrContext_onKeyEventNative(JNIEnv *env, jobject instance, jlong handle, jint keyCode,
                                            jint action) {
    return ((VrContext *)handle)->onKeyEvent(keyCode, action);
}