#include "AudioMATVController.h"
#include "AudioIoctl.h"

#include <utils/threads.h>

#include <cutils/properties.h>

#include "AudioMATVResourceManager.h"
#include "AudioMTKStreamManager.h"

#define LOG_TAG "AudioMATVController"

#ifdef MATV_AUDIO_SUPPORT
extern int matv_use_analog_input; //from libaudiosetting.so
#endif

namespace android
{

/*==============================================================================
 *                     Property keys
 *============================================================================*/


/*==============================================================================
 *                     Const Value
 *============================================================================*/


/*==============================================================================
 *                     Enumerator
 *============================================================================*/


/*==============================================================================
 *                     Singleton Pattern
 *============================================================================*/

AudioMATVController *AudioMATVController::mAudioMATVController = NULL;

AudioMATVController *AudioMATVController::GetInstance()
{
    static Mutex mGetInstanceLock;
    Mutex::Autolock _l(mGetInstanceLock);

    if (mAudioMATVController == NULL)
    {
        mAudioMATVController = new AudioMATVController();
    }
    ASSERT(mAudioMATVController != NULL);
    return mAudioMATVController;
}

/*==============================================================================
 *                     Constructor / Destructor / Init / Deinit
 *============================================================================*/

AudioMATVController::AudioMATVController()
{
    ALOGD("%s()", __FUNCTION__);

    mMatvEnable = false;
    mMatvSourceType = MATV_DIGITAL;
    mIsMatvDirectConnectionMode = false;

    mAudioMatvResourceManager = new AudioMATVResourceManager();
    mAudioMTKStreamManager = AudioMTKStreamManager::getInstance();
}

AudioMATVController::~AudioMATVController()
{
    ALOGD("%s()", __FUNCTION__);

    delete mAudioMatvResourceManager;
}

/*==============================================================================
 *                     MATV Control
 *============================================================================*/

bool AudioMATVController::GetMatvEnable()
{
    Mutex::Autolock _l(mLock);
    ALOGD("%s(), mMatvEnable = %d", __FUNCTION__, mMatvEnable);
    return mMatvEnable;
}

MATVTYPE AudioMATVController::GetMatvType()
{
    Mutex::Autolock _l(mLock);
    ALOGD("%s(), mMatvSourceType = %d", __FUNCTION__, mMatvSourceType);
    return mMatvSourceType;
}

status_t AudioMATVController::SetMatvEnable(bool enable, const MATVTYPE matv_type)
{
    // Lock to Protect HW Registers & AudioMode
    mAudioMatvResourceManager->EnableAudioLock(AudioResourceManagerInterface::AUDIO_HARDWARE_LOCK, 3000);
    mAudioMatvResourceManager->EnableAudioLock(AudioResourceManagerInterface::AUDIO_MODE_LOCK,     3000);

    Mutex::Autolock _l(mLock);

    ALOGD("+%s(), mMatvEnable = %d => enable = %d", __FUNCTION__, mMatvEnable, enable);

    // Check Current Status
    if (enable == mMatvEnable)
    {
        ALOGW("-%s(), enable == mMatvEnable, return.", __FUNCTION__);
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_MODE_LOCK);
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_HARDWARE_LOCK);
        return INVALID_OPERATION;
    }

    // Check enabled Matv path while disabling
    if ((enable == false) && (mMatvSourceType != matv_type))
    {

        if (matv_type)
        {
            ALOGW("-%s(), MatvDigital is Enabled, can't Disable MatvAnalog, return.", __FUNCTION__);
        }
        else
        {
            ALOGW("-%s(), MatvAnalog is Enabled, can't Disable MatvDigital, return.", __FUNCTION__);
        }
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_MODE_LOCK);
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_HARDWARE_LOCK);
        return INVALID_OPERATION;
    }

    // Check Audio Mode is Normal
    const audio_mode_t audio_mode = mAudioMatvResourceManager->GetAudioMode();
    if (audio_mode != AUDIO_MODE_NORMAL)
    {
        ALOGW("%s(), Current AudioMode(%d) is not AUDIO_MODE_NORMAL(%d), return.", __FUNCTION__, audio_mode, AUDIO_MODE_NORMAL);
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_MODE_LOCK);
        mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_HARDWARE_LOCK);
        return INVALID_OPERATION;
    }


    // Update Enable Status
    mMatvEnable = enable;

    // Get file descriptor for ioctl
    int fd_audio = ::open(kAudioDeviceName, O_RDWR);
    ALOGD("%s(), open(%s), fd_audio = %d", __FUNCTION__, kAudioDeviceName, fd_audio);
    ASSERT(fd_audio >= 0);

    // get current device
    const audio_devices_t output_device = (audio_devices_t)mAudioMatvResourceManager->getDlOutputDevice();
    ALOGD("%s(), output_device = 0x%x", __FUNCTION__, output_device);

    if (mMatvEnable == true) // Open
    {
        mMatvSourceType = matv_type;
        if (matv_type == MATV_ANALOG)
        {
            // Clock
            mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_AFE, true);
            mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_ANA, true);
            //Enable adc?
            // set sampling rate
            mAudioMatvResourceManager->SetFrequency(AudioResourceManagerInterface::DEVICE_OUT_DAC, GetMatvDownlinkSamplingRate());

            // AFE ON
            mAudioMatvResourceManager->SetAfeEnable(true);
            //Set MATV to analog
            mAudioMatvResourceManager->SetMatvSourceType(mMatvSourceType);

            // Set MATV source module enable
            mAudioMatvResourceManager->SetMatvSourceModuleEnable(true);

        }
        else
        {
        // Clock
        mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_AFE, true);
        mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_ANA, true);

        // set sampling rate
        mAudioMatvResourceManager->SetFrequency(AudioResourceManagerInterface::DEVICE_OUT_DAC, GetMatvDownlinkSamplingRate());

        // AFE ON
        mAudioMatvResourceManager->SetAfeEnable(true);

        // Set MATV chip I2S sampling rate and GPIO
        ::ioctl(fd_audio, AUDDRV_ENABLE_ATV_I2S_GPIO);  // Set ATV I2S path
            //Set MATV to digital
            mAudioMatvResourceManager->SetMatvSourceType(mMatvSourceType);

        // Set MATV source module enable
        mAudioMatvResourceManager->SetMatvSourceModuleEnable(true);

        // Set Direct Mode HW: 2nd I2S -> DAC I2S Out
        if (mIsMatvDirectConnectionMode == true) // will never happen until mATV chip support 44.1k
        {
            mAudioMatvResourceManager->SetMatvDirectConnection(true);
            if (mAudioMTKStreamManager->IsOutPutStreamActive() == false)
            {
                mAudioMatvResourceManager->StartOutputDevice();
            }
        }
    }
    }
    else // Close
    {
        mMatvSourceType = matv_type;
        if (matv_type == true)
        {
            // Disable Audio Digital/Analog HW Register
            if (mAudioMTKStreamManager->IsOutPutStreamActive() == false)
            {
                mAudioMatvResourceManager->StopOutputDevice();
            }
            mAudioMatvResourceManager->SetMatvSourceType(mMatvSourceType);

            // Set MATV source module disable
            mAudioMatvResourceManager->SetMatvSourceModuleEnable(false);

            // AFE OFF
            mAudioMatvResourceManager->SetAfeEnable(false);

            // close Clock
            mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_ANA, false);
            mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_AFE, false);
        }
        else
        {
        // Disable Audio Digital/Analog HW Register
        if (mAudioMTKStreamManager->IsOutPutStreamActive() == false)
        {
            mAudioMatvResourceManager->StopOutputDevice();
        }
        mAudioMatvResourceManager->SetMatvDirectConnection(false);

            mAudioMatvResourceManager->SetMatvSourceType(mMatvSourceType);
        // Set MATV source module disable
        mAudioMatvResourceManager->SetMatvSourceModuleEnable(false);

        // Set MATV chip I2S sampling rate and GPIO
        ::ioctl(fd_audio, AUDDRV_DISABLE_ATV_I2S_GPIO);

        // AFE OFF
        mAudioMatvResourceManager->SetAfeEnable(false);

        // Clock
        mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_ANA, false);
        mAudioMatvResourceManager->EnableAudioClock(AudioResourceManagerInterface::CLOCK_AUD_AFE, false);
    }
        //Set MATV to default digital
        mMatvSourceType = MATV_DIGITAL;
        mAudioMatvResourceManager->SetMatvSourceType(mMatvSourceType);
    }

    // Close file descriptor
    if (fd_audio >= 0)
    close(fd_audio);

    // Unlock
    mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_MODE_LOCK);
    mAudioMatvResourceManager->DisableAudioLock(AudioResourceManagerInterface::AUDIO_HARDWARE_LOCK);

    ALOGD("-%s()", __FUNCTION__);
    return NO_ERROR;
}

uint32_t AudioMATVController::GetMatvUplinkSamplingRate() const
{
    return mAudioMatvResourceManager->GetMatvUplinkSamplingRate();
}

uint32_t AudioMATVController::GetMatvDownlinkSamplingRate() const
{
    return mAudioMatvResourceManager->GetMatvDownlinkSamplingRate();
}

} // end of namespace android
