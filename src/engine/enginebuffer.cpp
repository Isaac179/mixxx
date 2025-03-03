#include "engine/enginebuffer.h"

#include <QtDebug>

#include "control/controlindicator.h"
#include "control/controllinpotmeter.h"
#include "control/controlpotmeter.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "engine/bufferscalers/enginebufferscalelinear.h"
#include "engine/bufferscalers/enginebufferscalerubberband.h"
#include "engine/bufferscalers/enginebufferscalest.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/channels/enginechannel.h"
#include "engine/controls/bpmcontrol.h"
#include "engine/controls/clockcontrol.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/enginecontrol.h"
#include "engine/controls/keycontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/controls/quantizecontrol.h"
#include "engine/controls/ratecontrol.h"
#include "engine/enginemaster.h"
#include "engine/engineworkerscheduler.h"
#include "engine/readaheadmanager.h"
#include "engine/sync/enginesync.h"
#include "engine/sync/synccontrol.h"
#include "moc_enginebuffer.cpp"
#include "preferences/usersettings.h"
#include "track/beatfactory.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/assert.h"
#include "util/compatibility.h"
#include "util/defs.h"
#include "util/logger.h"
#include "util/sample.h"
#include "util/timer.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveformwidgetfactory.h"

#ifdef __VINYLCONTROL__
#include "engine/controls/vinylcontrolcontrol.h"
#endif

namespace {
const mixxx::Logger kLogger("EngineBuffer");

constexpr double kLinearScalerElipsis =
        1.00058; // 2^(0.01/12): changes < 1 cent allows a linear scaler

constexpr SINT kSamplesPerFrame = 2; // Engine buffer uses Stereo frames only

// Rate at which the playpos slider is updated
constexpr int kPlaypositionUpdateRate = 15; // updates per second

} // anonymous namespace

EngineBuffer::EngineBuffer(const QString& group,
        UserSettingsPointer pConfig,
        EngineChannel* pChannel,
        EngineMaster* pMixingEngine)
        : m_group(group),
          m_pConfig(pConfig),
          m_pLoopingControl(nullptr),
          m_pSyncControl(nullptr),
          m_pVinylControlControl(nullptr),
          m_pRateControl(nullptr),
          m_pBpmControl(nullptr),
          m_pKeyControl(nullptr),
          m_pReadAheadManager(nullptr),
          m_pReader(nullptr),
          m_filepos_play(kInitalSamplePosition),
          m_speed_old(0),
          m_tempo_ratio_old(1.),
          m_scratching_old(false),
          m_reverse_old(false),
          m_pitch_old(0),
          m_baserate_old(0),
          m_rate_old(0.),
          m_trackSamplesOld(0),
          m_dSlipPosition(0.),
          m_dSlipRate(1.0),
          m_bSlipEnabledProcessing(false),
          m_pRepeat(nullptr),
          m_startButton(nullptr),
          m_endButton(nullptr),
          m_bScalerOverride(false),
          m_iSeekPhaseQueued(0),
          m_iEnableSyncQueued(SYNC_REQUEST_NONE),
          m_iSyncModeQueued(static_cast<int>(SyncMode::Invalid)),
          m_iTrackLoading(0),
          m_bPlayAfterLoading(false),
          m_pCrossfadeBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)),
          m_bCrossfadeReady(false),
          m_iLastBufferSize(0) {
    m_queuedSeek.setValue(kNoQueuedSeek);

    // zero out crossfade buffer
    SampleUtil::clear(m_pCrossfadeBuffer, MAX_BUFFER_LEN);

    m_pReader = new CachingReader(group, pConfig);
    connect(m_pReader, &CachingReader::trackLoading,
            this, &EngineBuffer::slotTrackLoading,
            Qt::DirectConnection);
    connect(m_pReader, &CachingReader::trackLoaded,
            this, &EngineBuffer::slotTrackLoaded,
            Qt::DirectConnection);
    connect(m_pReader, &CachingReader::trackLoadFailed,
            this, &EngineBuffer::slotTrackLoadFailed,
            Qt::DirectConnection);

    // Play button
    m_playButton = new ControlPushButton(ConfigKey(m_group, "play"));
    m_playButton->setButtonMode(ControlPushButton::TOGGLE);
    m_playButton->connectValueChangeRequest(
            this, &EngineBuffer::slotControlPlayRequest,
            Qt::DirectConnection);

    //Play from Start Button (for sampler)
    m_playStartButton = new ControlPushButton(ConfigKey(m_group, "start_play"));
    connect(m_playStartButton, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlPlayFromStart,
            Qt::DirectConnection);

    // Jump to start and stop button
    m_stopStartButton = new ControlPushButton(ConfigKey(m_group, "start_stop"));
    connect(m_stopStartButton, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlJumpToStartAndStop,
            Qt::DirectConnection);

    //Stop playback (for sampler)
    m_stopButton = new ControlPushButton(ConfigKey(m_group, "stop"));
    connect(m_stopButton, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlStop,
            Qt::DirectConnection);

    // Start button
    m_startButton = new ControlPushButton(ConfigKey(m_group, "start"));
    m_startButton->setButtonMode(ControlPushButton::TRIGGER);
    connect(m_startButton, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlStart,
            Qt::DirectConnection);

    // End button
    m_endButton = new ControlPushButton(ConfigKey(m_group, "end"));
    connect(m_endButton, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlEnd,
            Qt::DirectConnection);

    m_pSlipButton = new ControlPushButton(ConfigKey(m_group, "slip_enabled"));
    m_pSlipButton->setButtonMode(ControlPushButton::TOGGLE);

    m_playposSlider = new ControlLinPotmeter(
        ConfigKey(m_group, "playposition"), 0.0, 1.0, 0, 0, true);
    connect(m_playposSlider, &ControlObject::valueChanged,
            this, &EngineBuffer::slotControlSeek,
            Qt::DirectConnection);

    // Control used to communicate ratio playpos to GUI thread
    m_visualPlayPos = VisualPlayPosition::getVisualPlayPosition(m_group);

    m_pRepeat = new ControlPushButton(ConfigKey(m_group, "repeat"));
    m_pRepeat->setButtonMode(ControlPushButton::TOGGLE);

    m_pSampleRate = new ControlProxy("[Master]", "samplerate", this);

    m_pKeylockEngine = new ControlProxy("[Master]", "keylock_engine", this);
    m_pKeylockEngine->connectValueChanged(this, &EngineBuffer::slotKeylockEngineChanged,
                                          Qt::DirectConnection);

    m_pTrackSamples = new ControlObject(ConfigKey(m_group, "track_samples"));
    m_pTrackSampleRate = new ControlObject(ConfigKey(m_group, "track_samplerate"));

    m_pKeylock = new ControlPushButton(ConfigKey(m_group, "keylock"), true);
    m_pKeylock->setButtonMode(ControlPushButton::TOGGLE);

    m_pEject = new ControlPushButton(ConfigKey(m_group, "eject"));
    connect(m_pEject, &ControlObject::valueChanged,
            this, &EngineBuffer::slotEjectTrack,
            Qt::DirectConnection);

    m_pTrackLoaded = new ControlObject(ConfigKey(m_group, "track_loaded"), false);
    m_pTrackLoaded->setReadOnly();

    // Quantization Controller for enabling and disabling the
    // quantization (alignment) of loop in/out positions and (hot)cues with
    // beats.
    QuantizeControl* quantize_control = new QuantizeControl(group, pConfig);
    addControl(quantize_control);
    m_pQuantize = ControlObject::getControl(ConfigKey(group, "quantize"));

    // Create the Loop Controller
    m_pLoopingControl = new LoopingControl(group, pConfig);
    addControl(m_pLoopingControl);

    m_pEngineSync = pMixingEngine->getEngineSync();

    m_pSyncControl = new SyncControl(group, pConfig, pChannel, m_pEngineSync);

#ifdef __VINYLCONTROL__
    m_pVinylControlControl = new VinylControlControl(group, pConfig);
    addControl(m_pVinylControlControl);
#endif

    // Create the Rate Controller
    m_pRateControl = new RateControl(group, pConfig);
    // Add the Rate Controller
    addControl(m_pRateControl);
    // Looping Control needs Rate Control for Reverse Button
    m_pLoopingControl->setRateControl(m_pRateControl);

    // Create the BPM Controller
    m_pBpmControl = new BpmControl(group, pConfig);
    addControl(m_pBpmControl);

    // TODO(rryan) remove this dependence?
    m_pRateControl->setBpmControl(m_pBpmControl);
    m_pSyncControl->setEngineControls(m_pRateControl, m_pBpmControl);
    pMixingEngine->getEngineSync()->addSyncableDeck(m_pSyncControl);
    addControl(m_pSyncControl);

    m_fwdButton = ControlObject::getControl(ConfigKey(group, "fwd"));
    m_backButton = ControlObject::getControl(ConfigKey(group, "back"));

    m_pKeyControl = new KeyControl(group, pConfig);
    addControl(m_pKeyControl);

    // Create the clock controller
    m_pClockControl = new ClockControl(group, pConfig);
    addControl(m_pClockControl);

    // Create the cue controller
    m_pCueControl = new CueControl(group, pConfig);
    addControl(m_pCueControl);

    connect(m_pLoopingControl,
            &LoopingControl::loopReset,
            m_pCueControl,
            &CueControl::slotLoopReset,
            Qt::DirectConnection);
    connect(m_pLoopingControl,
            &LoopingControl::loopUpdated,
            m_pCueControl,
            &CueControl::slotLoopUpdated,
            Qt::DirectConnection);
    connect(m_pLoopingControl,
            &LoopingControl::loopEnabledChanged,
            m_pCueControl,
            &CueControl::slotLoopEnabledChanged,
            Qt::DirectConnection);

    m_pReadAheadManager = new ReadAheadManager(m_pReader,
                                               m_pLoopingControl);
    m_pReadAheadManager->addRateControl(m_pRateControl);

    // Construct scaling objects
    m_pScaleLinear = new EngineBufferScaleLinear(m_pReadAheadManager);
    m_pScaleST = new EngineBufferScaleST(m_pReadAheadManager);
    m_pScaleRB = new EngineBufferScaleRubberBand(m_pReadAheadManager);
    if (m_pKeylockEngine->get() == SOUNDTOUCH) {
        m_pScaleKeylock = m_pScaleST;
    } else {
        m_pScaleKeylock = m_pScaleRB;
    }
    m_pScaleVinyl = m_pScaleLinear;
    m_pScale = m_pScaleVinyl;
    m_pScale->clear();
    m_bScalerChanged = true;

    m_pPassthroughEnabled = new ControlProxy(group, "passthrough", this);
    m_pPassthroughEnabled->connectValueChanged(this, &EngineBuffer::slotPassthroughChanged,
                                               Qt::DirectConnection);

#ifdef __SCALER_DEBUG__
    df.setFileName("mixxx-debug.csv");
    df.open(QIODevice::WriteOnly | QIODevice::Text);
    writer.setDevice(&df);
#endif

    // Now that all EngineControls have been created call setEngineMaster.
    // TODO(XXX): Get rid of EngineControl::setEngineMaster and
    // EngineControl::setEngineBuffer entirely and pass them through the
    // constructor.
    setEngineMaster(pMixingEngine);
}

EngineBuffer::~EngineBuffer() {
#ifdef __SCALER_DEBUG__
    //close the writer
    df.close();
#endif
    delete m_pReadAheadManager;
    delete m_pReader;

    delete m_playButton;
    delete m_playStartButton;
    delete m_stopStartButton;

    delete m_startButton;
    delete m_endButton;
    delete m_stopButton;
    delete m_playposSlider;

    delete m_pSlipButton;
    delete m_pRepeat;
    delete m_pSampleRate;

    delete m_pTrackLoaded;
    delete m_pTrackSamples;
    delete m_pTrackSampleRate;

    delete m_pScaleLinear;
    delete m_pScaleST;
    delete m_pScaleRB;

    delete m_pKeylock;
    delete m_pEject;

    SampleUtil::free(m_pCrossfadeBuffer);

    qDeleteAll(m_engineControls);
}

void EngineBuffer::bindWorkers(EngineWorkerScheduler* pWorkerScheduler) {
    m_pReader->setScheduler(pWorkerScheduler);
}

void EngineBuffer::enableIndependentPitchTempoScaling(bool bEnable,
                                                      const int iBufferSize) {
    // MUST ACQUIRE THE PAUSE MUTEX BEFORE CALLING THIS METHOD

    // When no time-stretching or pitch-shifting is needed we use our own linear
    // interpolation code (EngineBufferScaleLinear). It is faster and sounds
    // much better for scratching.

    // m_pScaleKeylock and m_pScaleVinyl could change out from under us,
    // so cache it.
    EngineBufferScale* keylock_scale = m_pScaleKeylock;
    EngineBufferScale* vinyl_scale = m_pScaleVinyl;

    if (bEnable && m_pScale != keylock_scale) {
        if (m_speed_old != 0.0) {
            // Crossfade if we are not paused.
            // If we start from zero a ramping gain is
            // applied later
            readToCrossfadeBuffer(iBufferSize);
        }
        m_pScale = keylock_scale;
        m_pScale->clear();
        m_bScalerChanged = true;
    } else if (!bEnable && m_pScale != vinyl_scale) {
        if (m_speed_old != 0.0) {
            // Crossfade if we are not paused
            // (for slow speeds below 0.1 the vinyl_scale is used)
            readToCrossfadeBuffer(iBufferSize);
        }
        m_pScale = vinyl_scale;
        m_pScale->clear();
        m_bScalerChanged = true;
    }
}

mixxx::Bpm EngineBuffer::getBpm() const {
    return m_pBpmControl->getBpm();
}

mixxx::Bpm EngineBuffer::getLocalBpm() const {
    return m_pBpmControl->getLocalBpm();
}

void EngineBuffer::setBeatLoop(mixxx::audio::FramePos startPosition, bool enabled) {
    return m_pLoopingControl->setBeatLoop(startPosition, enabled);
}

void EngineBuffer::setLoop(mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPositon,
        bool enabled) {
    return m_pLoopingControl->setLoop(startPosition, endPositon, enabled);
}

void EngineBuffer::setEngineMaster(EngineMaster* pEngineMaster) {
    for (const auto& pControl: qAsConst(m_engineControls)) {
        pControl->setEngineMaster(pEngineMaster);
    }
}

void EngineBuffer::queueNewPlaypos(mixxx::audio::FramePos position, enum SeekRequest seekType) {
    // All seeks need to be done in the Engine thread so queue it up.
    // Write the position before the seek type, to reduce a possible race
    // condition effect
    VERIFY_OR_DEBUG_ASSERT(seekType != SEEK_PHASE) {
        // SEEK_PHASE with a position is not supported
        // use SEEK_STANDARD for that
        seekType = SEEK_STANDARD;
    }
    m_queuedSeek.setValue({position, seekType});
}

void EngineBuffer::requestSyncPhase() {
    // Don't overwrite m_iSeekQueued
    m_iSeekPhaseQueued = 1;
}

void EngineBuffer::requestEnableSync(bool enabled) {
    // If we're not playing, the queued event won't get processed so do it now.
    if (m_playButton->get() == 0.0) {
        if (enabled) {
            m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::Follower);
        } else {
            m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::None);
        }
        return;
    }
    SyncRequestQueued enable_request =
            static_cast<SyncRequestQueued>(atomicLoadRelaxed(m_iEnableSyncQueued));
    if (enabled) {
        m_iEnableSyncQueued = SYNC_REQUEST_ENABLE;
    } else {
        // If sync is enabled and disabled very quickly, it's is a one-shot
        // sync event and needs to be handled specially. Otherwise the sync
        // state will get stuck on or won't go on at all.
        if (enable_request == SYNC_REQUEST_ENABLE) {
            m_iEnableSyncQueued = SYNC_REQUEST_ENABLEDISABLE;
        } else {
            // Note that there is no DISABLEENABLE, because that's an irrelevant
            // queuing.  Moreover, ENABLEDISABLEENABLE is also redundant, so
            // we don't have to handle any special cases.
            m_iEnableSyncQueued = SYNC_REQUEST_DISABLE;
        }
    }
}

void EngineBuffer::requestSyncMode(SyncMode mode) {
    // If we're not playing, the queued event won't get processed so do it now.
    if (kLogger.traceEnabled()) {
        kLogger.trace() << getGroup() << "EngineBuffer::requestSyncMode";
    }
    if (m_playButton->get() == 0.0) {
        m_pEngineSync->requestSyncMode(m_pSyncControl, mode);
    } else {
        m_iSyncModeQueued = static_cast<int>(mode);
    }
}

void EngineBuffer::requestClonePosition(EngineChannel* pChannel) {
    atomicStoreRelaxed(m_pChannelToCloneFrom, pChannel);
}

void EngineBuffer::readToCrossfadeBuffer(const int iBufferSize) {
    if (!m_bCrossfadeReady) {
        // Read buffer, as if there where no parameter change
        // (Must be called only once per callback)
        m_pScale->scaleBuffer(m_pCrossfadeBuffer, iBufferSize);
        // Restore the original position that was lost due to scaleBuffer() above
        m_pReadAheadManager->notifySeek(m_filepos_play);
        m_bCrossfadeReady = true;
     }
}

void EngineBuffer::seekCloneBuffer(EngineBuffer* pOtherBuffer) {
    const auto position = mixxx::audio::FramePos::fromEngineSamplePos(
            pOtherBuffer->getExactPlayPos());
    doSeekPlayPos(position, SEEK_EXACT);
}

// WARNING: This method is not thread safe and must not be called from outside
// the engine callback!
void EngineBuffer::setNewPlaypos(double newpos) {
    if (kLogger.traceEnabled()) {
        kLogger.trace() << m_group << "EngineBuffer::setNewPlaypos" << newpos;
    }

    m_filepos_play = newpos;

    if (m_rate_old != 0.0) {
        // Before seeking, read extra buffer for crossfading
        // this also sets m_pReadAheadManager to newpos
        readToCrossfadeBuffer(m_iLastBufferSize);
    } else {
        m_pReadAheadManager->notifySeek(m_filepos_play);
    }
    m_pScale->clear();

    // Ensures that the playpos slider gets updated in next process call
    m_iSamplesSinceLastIndicatorUpdate = 1000000;

    // Must hold the engineLock while using m_engineControls
    const auto playPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_filepos_play);
    for (const auto& pControl: qAsConst(m_engineControls)) {
        pControl->notifySeek(playPosition);
    }

    verifyPlay(); // verify or update play button and indicator
}

QString EngineBuffer::getGroup() const {
    return m_group;
}

double EngineBuffer::getSpeed() const {
    return m_speed_old;
}

bool EngineBuffer::getScratching() const {
    return m_scratching_old;
}

bool EngineBuffer::isReverse() const {
    return m_reverse_old;
}

// WARNING: Always called from the EngineWorker thread pool
void EngineBuffer::slotTrackLoading() {
    // Pause EngineBuffer from processing frames
    m_pause.lock();
    // Setting m_iTrackLoading inside a m_pause.lock ensures that
    // track buffer is not processed when starting to load a new one
    m_iTrackLoading = 1;
    m_pause.unlock();

    // Set play here, to signal the user that the play command is adopted
    m_playButton->set((double)m_bPlayAfterLoading);
    m_pTrackSamples->set(0); // Stop renderer
}

void EngineBuffer::loadFakeTrack(TrackPointer pTrack, bool bPlay) {
    if (bPlay) {
        m_playButton->set((double)bPlay);
    }
    slotTrackLoaded(
            pTrack,
            pTrack->getSampleRate(),
            // TODO: Round to integer after multiplication with sample rate
            // and not before?
            pTrack->getSampleRate() * pTrack->getDurationSecondsInt());
}

// WARNING: Always called from the EngineWorker thread pool
void EngineBuffer::slotTrackLoaded(TrackPointer pTrack,
                                   int iTrackSampleRate,
                                   int iTrackNumSamples) {
    if (kLogger.traceEnabled()) {
        kLogger.trace() << getGroup() << "EngineBuffer::slotTrackLoaded";
    }
    TrackPointer pOldTrack = m_pCurrentTrack;
    m_pause.lock();

    m_visualPlayPos->setInvalid();
    m_filepos_play = kInitalSamplePosition; // for execute seeks to 0.0
    m_pCurrentTrack = pTrack;
    m_pTrackSamples->set(iTrackNumSamples);
    m_pTrackSampleRate->set(iTrackSampleRate);
    m_pTrackLoaded->forceSet(1);

    // Reset slip mode
    m_pSlipButton->set(0);
    m_bSlipEnabledProcessing = false;
    m_dSlipPosition = 0.;
    m_dSlipRate = 0;

    m_queuedSeek.setValue(kNoQueuedSeek);

    // Reset the pitch value for the new track.
    m_pause.unlock();

    notifyTrackLoaded(pTrack, pOldTrack);
    // Start buffer processing after all EngineContols are up to date
    // with the current track e.g track is seeked to Cue
    m_iTrackLoading = 0;
}

// WARNING: Always called from the EngineWorker thread pool
void EngineBuffer::slotTrackLoadFailed(TrackPointer pTrack,
        const QString& reason) {
    m_iTrackLoading = 0;
    // Loading of a new track failed.
    // eject the currently loaded track (the old Track) as well
    ejectTrack();
    emit trackLoadFailed(pTrack, reason);
}

void EngineBuffer::ejectTrack() {
    // clear track values in any case, may fix https://bugs.launchpad.net/mixxx/+bug/1450424
    if (kLogger.traceEnabled()) {
        kLogger.trace() << "EngineBuffer::ejectTrack()";
    }
    TrackPointer pOldTrack = m_pCurrentTrack;
    m_pause.lock();

    m_visualPlayPos->set(0.0, 0.0, 0.0, 0.0, 0.0);
    doSeekPlayPos(mixxx::audio::kStartFramePos, SEEK_EXACT);

    m_pCurrentTrack.reset();
    m_pTrackSamples->set(0);
    m_pTrackSampleRate->set(0);
    m_pTrackLoaded->forceSet(0);

    m_playButton->set(0.0);
    m_playposSlider->set(0);
    m_pCueControl->resetIndicators();

    m_queuedSeek.setValue(kNoQueuedSeek);

    m_pause.unlock();

    // Close open file handles by unloading the current track
    m_pReader->newTrack(TrackPointer());

    if (pOldTrack) {
        notifyTrackLoaded(TrackPointer(), pOldTrack);
    }
    m_iTrackLoading = 0;
}

void EngineBuffer::notifyTrackLoaded(
        TrackPointer pNewTrack, TrackPointer pOldTrack) {
    if (pOldTrack) {
        disconnect(
                pOldTrack.get(),
                &Track::beatsUpdated,
                this,
                &EngineBuffer::slotUpdatedTrackBeats);
    }

    // First inform engineControls directly
    // Note: we are still in a worker thread.
    const auto currentPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_filepos_play);
    const auto trackEndPosition =
            mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                    m_pTrackSamples->get());
    const auto sampleRate = mixxx::audio::SampleRate::fromDouble(m_pTrackSampleRate->get());
    for (const auto& pControl : qAsConst(m_engineControls)) {
        pControl->trackLoaded(pNewTrack);
        pControl->setFrameInfo(currentPosition, trackEndPosition, sampleRate);
    }

    if (pNewTrack) {
        connect(
                pNewTrack.get(),
                &Track::beatsUpdated,
                this,
                &EngineBuffer::slotUpdatedTrackBeats,
                Qt::DirectConnection);
    }

    // Inform BaseTrackPlayer via a queued connection
    emit trackLoaded(pNewTrack, pOldTrack);
}

void EngineBuffer::slotPassthroughChanged(double enabled) {
    if (enabled != 0) {
        // If passthrough was enabled, stop playing the current track.
        slotControlStop(1.0);
    }
}

// WARNING: This method runs in both the GUI thread and the Engine Thread
void EngineBuffer::slotControlSeek(double fractionalPos) {
    doSeekFractional(fractionalPos, SEEK_STANDARD);
}

// WARNING: This method runs from SyncWorker and Engine Worker
void EngineBuffer::slotControlSeekAbs(double playPosition) {
    // TODO: Check if we can assert a valid play position here
    const auto position = mixxx::audio::FramePos::fromEngineSamplePos(playPosition);
    doSeekPlayPos(position, SEEK_STANDARD);
}

// WARNING: This method runs from SyncWorker and Engine Worker
void EngineBuffer::slotControlSeekExact(double playPosition) {
    // TODO: Check if we can assert a valid play position here
    const auto position = mixxx::audio::FramePos::fromEngineSamplePos(playPosition);
    doSeekPlayPos(position, SEEK_EXACT);
}

double EngineBuffer::fractionalPlayposFromAbsolute(double absolutePlaypos) {
    double fFractionalPlaypos = 0.0;
    if (m_trackSamplesOld != 0) {
        fFractionalPlaypos = math_min<double>(absolutePlaypos, m_trackSamplesOld);
        fFractionalPlaypos /= m_trackSamplesOld;
    }
    return fFractionalPlaypos;
}

void EngineBuffer::doSeekFractional(double fractionalPos, enum SeekRequest seekType) {
    // Prevent NaN's from sneaking into the engine.
    VERIFY_OR_DEBUG_ASSERT(!util_isnan(fractionalPos)) {
        return;
    }

    // FIXME: Use maybe invalid here
    const auto trackEndPosition =
            mixxx::audio::FramePos::fromEngineSamplePos(m_pTrackSamples->get());
    VERIFY_OR_DEBUG_ASSERT(trackEndPosition.isValid()) {
        return;
    }
    const auto seekPosition = trackEndPosition * fractionalPos;
    doSeekPlayPos(seekPosition, seekType);
}

void EngineBuffer::doSeekPlayPos(mixxx::audio::FramePos position, enum SeekRequest seekType) {
#ifdef __VINYLCONTROL__
    // Notify the vinyl control that a seek has taken place in case it is in
    // absolute mode and needs be switched to relative.
    if (m_pVinylControlControl) {
        m_pVinylControlControl->notifySeekQueued();
    }
#endif

    queueNewPlaypos(position, seekType);
}

bool EngineBuffer::updateIndicatorsAndModifyPlay(bool newPlay, bool oldPlay) {
    // If no track is currently loaded, turn play off. If a track is loading
    // allow the set since it might apply to a track we are loading due to the
    // asynchrony.
    bool playPossible = true;
    const QueuedSeek queuedSeek = m_queuedSeek.getValue();
    if ((!m_pCurrentTrack && atomicLoadRelaxed(m_iTrackLoading) == 0) ||
            (m_pCurrentTrack && atomicLoadRelaxed(m_iTrackLoading) == 0 &&
                    m_filepos_play >= m_pTrackSamples->get() &&
                    queuedSeek.seekType == SEEK_NONE) ||
            m_pPassthroughEnabled->toBool()) {
        // play not possible
        playPossible = false;
    }

    return m_pCueControl->updateIndicatorsAndModifyPlay(newPlay, oldPlay, playPossible);
}

void EngineBuffer::verifyPlay() {
    bool play = m_playButton->toBool();
    bool verifiedPlay = updateIndicatorsAndModifyPlay(play, play);
    if (play != verifiedPlay) {
        m_playButton->setAndConfirm(verifiedPlay ? 1.0 : 0.0);
    }
}

void EngineBuffer::slotControlPlayRequest(double v) {
    bool oldPlay = m_playButton->toBool();
    bool verifiedPlay = updateIndicatorsAndModifyPlay(v > 0.0, oldPlay);

    if (!oldPlay && verifiedPlay) {
        if (m_pQuantize->toBool()
#ifdef __VINYLCONTROL__
                && m_pVinylControlControl && !m_pVinylControlControl->isEnabled()
#endif
        ) {
            requestSyncPhase();
        }
    }

    // set and confirm must be called here in any case to update the widget toggle state
    m_playButton->setAndConfirm(verifiedPlay ? 1.0 : 0.0);
}

void EngineBuffer::slotControlStart(double v)
{
    if (v > 0.0) {
        doSeekFractional(0., SEEK_EXACT);
    }
}

void EngineBuffer::slotControlEnd(double v)
{
    if (v > 0.0) {
        doSeekFractional(1., SEEK_EXACT);
    }
}

void EngineBuffer::slotControlPlayFromStart(double v)
{
    if (v > 0.0) {
        doSeekFractional(0., SEEK_EXACT);
        m_playButton->set(1);
    }
}

void EngineBuffer::slotControlJumpToStartAndStop(double v)
{
    if (v > 0.0) {
        doSeekFractional(0., SEEK_EXACT);
        m_playButton->set(0);
    }
}

void EngineBuffer::slotControlStop(double v)
{
    if (v > 0.0) {
        m_playButton->set(0);
    }
}

void EngineBuffer::slotKeylockEngineChanged(double dIndex) {
    if (m_bScalerOverride) {
        return;
    }
    // static_cast<KeylockEngine>(dIndex); direct cast produces a "not used" warning with gcc
    int iEngine = static_cast<int>(dIndex);
    KeylockEngine engine = static_cast<KeylockEngine>(iEngine);
    if (engine == SOUNDTOUCH) {
        m_pScaleKeylock = m_pScaleST;
    } else {
        m_pScaleKeylock = m_pScaleRB;
    }
}

void EngineBuffer::processTrackLocked(
        CSAMPLE* pOutput, const int iBufferSize, mixxx::audio::SampleRate sampleRate) {
    ScopedTimer t("EngineBuffer::process_pauselock");

    m_trackSampleRateOld = mixxx::audio::SampleRate::fromDouble(m_pTrackSampleRate->get());
    m_trackSamplesOld = m_pTrackSamples->get();

    double baserate = 0.0;
    if (sampleRate.isValid()) {
        baserate = m_trackSampleRateOld / sampleRate;
    }

    // Sync requests can affect rate, so process those first.
    processSyncRequests();

    // Note: play is also active during cue preview
    bool paused = !m_playButton->toBool();
    KeyControl::PitchTempoRatio pitchTempoRatio = m_pKeyControl->getPitchTempoRatio();

    // The pitch adjustment in Ratio (1.0 being normal
    // pitch. 2.0 is a full octave shift up).
    double pitchRatio = pitchTempoRatio.pitchRatio;
    double tempoRatio = pitchTempoRatio.tempoRatio;
    const bool keylock_enabled = pitchTempoRatio.keylock;

    bool is_scratching = false;
    bool is_reverse = false;

    // Update the slipped position and seek if it was disabled.
    processSlip(iBufferSize);

    // Note: This may effects the m_filepos_play, play, scaler and crossfade buffer
    processSeek(paused);

    // speed is the ratio between track-time and real-time
    // (1.0 being normal rate. 2.0 plays at 2x speed -- 2 track seconds
    // pass for every 1 real second). Depending on whether
    // keylock is enabled, this is applied to either the rate or the tempo.
    double speed = m_pRateControl->calculateSpeed(
            baserate,
            tempoRatio,
            paused,
            iBufferSize,
            &is_scratching,
            &is_reverse);

    bool useIndependentPitchAndTempoScaling = false;

    // TODO(owen): Maybe change this so that rubberband doesn't disable
    // keylock on scratch. (just check m_pScaleKeylock == m_pScaleST)
    if (is_scratching || fabs(speed) > 1.9) {
        // Scratching and high speeds with always disables keylock
        // because Soundtouch sounds terrible in these conditions.  Rubberband
        // sounds better, but still has some problems (it may reallocate in
        // a party-crashing manner at extremely slow speeds).
        // High seek speeds also disables keylock.  Our pitch slider could go
        // to 90%, so that's the cutoff point.

        // Force pitchRatio to the linear pitch set by speed
        pitchRatio = speed;
        // This is for the natural speed pitch found on turn tables
    } else if (fabs(speed) < 0.1) {
        // We have pre-allocated big buffers in Rubberband and Soundtouch for
        // a minimum speed of 0.1. Slower speeds will re-allocate much bigger
        // buffers which may cause underruns.
        // Disable keylock under these conditions.

        // Force pitchRatio to the linear pitch set by speed
        pitchRatio = speed;
    } else if (keylock_enabled) {
        // always use IndependentPitchAndTempoScaling
        // to avoid clicks when crossing the linear pitch
        // in this case it is most likely that the user
        // will have an non linear pitch
        // Note: We have undesired noise when cossfading between scalers
        useIndependentPitchAndTempoScaling = true;
    } else {
        // We might have have temporary speed change, so adjust pitch if not locked
        // Note: This will not update key and tempo widgets
        if (tempoRatio != 0) {
            pitchRatio *= (speed / tempoRatio);
        }

        // Check if we are off-linear (musical key has been adjusted
        // independent from speed) to determine if the keylock scaler
        // should be used even though keylock is disabled.
        if (speed != 0.0) {
            double offlinear = pitchRatio / speed;
            if (offlinear > kLinearScalerElipsis ||
                    offlinear < 1 / kLinearScalerElipsis) {
                // only enable keylock scaler if pitch adjustment is at
                // least 1 cent. Everything below is not hear-able.
                useIndependentPitchAndTempoScaling = true;
            }
        }
    }

    if (speed != 0.0) {
        // Do not switch scaler when we have no transport
        enableIndependentPitchTempoScaling(useIndependentPitchAndTempoScaling,
                iBufferSize);
    } else if (m_speed_old != 0 && !is_scratching) {
        // we are stopping, collect samples for fade out
        readToCrossfadeBuffer(iBufferSize);
        // Clear the scaler information
        m_pScale->clear();
    }

    // How speed/tempo/pitch are related:
    // Processing is done in two parts, the first part is calculated inside
    // the KeyKontrol class and effects the visual key/pitch widgets.
    // The Speed slider controls the tempoRatio and a speedSliderPitchRatio,
    // the pitch amount caused by it.
    // By default the speed slider controls pitch and tempo with the same
    // value.
    // If key lock is enabled, the speedSliderPitchRatio is decoupled from
    // the speed slider (const).
    //
    // With preference mode KeylockMode = kLockOriginalKey
    // the speedSliderPitchRatio is reset to 1 and back to the tempoRatio
    // (natural vinyl Pitch) when keylock is disabled and enabled.
    //
    // With preference mode KeylockMode = kCurrentKey
    // the speedSliderPitchRatio is not reset when keylock is enabled.
    // This mode allows to enable keylock
    // while the track is already played. You can reset to the tracks
    // original pitch by resetting the pitch knob to center. When disabling
    // keylock the pitch is reset to the linear vinyl pitch.

    // The Pitch knob turns if the speed slider is moved without keylock.
    // This is useful to get always an analog impression of current pitch,
    // and its distance to the original track pitch
    //
    // The Pitch_Adjust knob does not reflect the speedSliderPitchRatio.
    // So it is is useful for controller mappings, because it is not
    // changed by the speed slider or keylock.

    // In the second part all other speed changing controls are processed.
    // They may produce an additional pitch if keylock is disabled or
    // override the pitch in scratching case.
    // If pitch ratio and tempo ratio are equal, a linear scaler is used,
    // otherwise tempo and pitch are processed individual

    // If we were scratching, and scratching is over, and we're a follower,
    // and we're quantized, and not paused,
    // we need to sync phase or we'll be totally out of whack and the sync
    // adjuster will kick in and push the track back in to sync with the
    // master.
    if (m_scratching_old && !is_scratching && m_pQuantize->toBool() &&
            isFollower(m_pSyncControl->getSyncMode()) && !paused) {
        // TODO() The resulting seek is processed in the following callback
        // That is to late
        requestSyncPhase();
    }

    double rate = 0;
    // If the baserate, speed, or pitch has changed, we need to update the
    // scaler. Also, if we have changed scalers then we need to update the
    // scaler.
    if (baserate != m_baserate_old || speed != m_speed_old ||
            pitchRatio != m_pitch_old || tempoRatio != m_tempo_ratio_old ||
            m_bScalerChanged) {
        // The rate returned by the scale object can be different from the
        // wanted rate!  Make sure new scaler has proper position. This also
        // crossfades between the old scaler and new scaler to prevent
        // clicks.

        // Handle direction change.
        // The linear scaler supports ramping though zero.
        // This is used for scratching, but not for reverse
        // For the other, crossfade forward and backward samples
        if ((m_speed_old * speed < 0) &&  // Direction has changed!
                (m_pScale != m_pScaleVinyl || // only m_pScaleLinear supports going though 0
                       m_reverse_old != is_reverse)) { // no pitch change when reversing
            //XXX: Trying to force RAMAN to read from correct
            //     playpos when rate changes direction - Albert
            readToCrossfadeBuffer(iBufferSize);
            // Clear the scaler information
            m_pScale->clear();
        }

        m_baserate_old = baserate;
        m_speed_old = speed;
        m_pitch_old = pitchRatio;
        m_tempo_ratio_old = tempoRatio;
        m_reverse_old = is_reverse;

        // Now we need to update the scaler with the master sample rate, the
        // base rate (ratio between sample rate of the source audio and the
        // master samplerate), the deck speed, the pitch shift, and whether
        // the deck speed should affect the pitch.

        m_pScale->setScaleParameters(baserate,
                                     &speed,
                                     &pitchRatio);

        // The way we treat rate inside of EngineBuffer is actually a
        // description of "sample consumption rate" or percentage of samples
        // consumed relative to playing back the track at its native sample
        // rate and normal speed. pitch_adjust does not change the playback
        // rate.
        rate = baserate * speed;

        // Scaler is up to date now.
        m_bScalerChanged = false;
    } else {
        // Scaler did not need updating. By definition this means we are at
        // our old rate.
        rate = m_rate_old;
    }

    bool at_end = m_filepos_play >= m_trackSamplesOld;
    bool backwards = rate < 0;

    bool bCurBufferPaused = false;
    if (at_end && !backwards) {
        // do not play past end
        bCurBufferPaused = true;
    } else if (rate == 0 && !is_scratching) {
        // do not process samples if have no transport
        // the linear scaler supports ramping down to 0
        // this is used for pause by scratching only
        bCurBufferPaused = true;
    }

    m_rate_old = rate;

    // If the buffer is not paused, then scale the audio.
    if (!bCurBufferPaused) {
        // Perform scaling of Reader buffer into buffer.
        double framesRead =
                m_pScale->scaleBuffer(pOutput, iBufferSize);

        // TODO(XXX): The result framesRead might not be an integer value.
        // Converting to samples here does not make sense. All positional
        // calculations should be done in frames instead of samples! Otherwise
        // rounding errors might occur when converting from samples back to
        // frames later.
        double samplesRead = framesRead * kSamplesPerFrame;

        if (m_bScalerOverride) {
            // If testing, we don't have a real log so we fake the position.
            m_filepos_play += samplesRead;
        } else {
            // Adjust filepos_play by the amount we processed.
            m_filepos_play =
                    m_pReadAheadManager->getFilePlaypositionFromLog(
                            m_filepos_play, samplesRead);
        }
        // Note: The last buffer of a track is padded with silence.
        // This silence is played together with the last samples in the last
        // callback and the m_filepos_play is advanced behind the end of the track.

        if (m_bCrossfadeReady) {
            // Bring pOutput with the new parameters in and fade out the old one,
            // stored with the old parameters in m_pCrossfadeBuffer
            SampleUtil::linearCrossfadeBuffersIn(
                    pOutput, m_pCrossfadeBuffer, iBufferSize);
        }
        // Note: we do not fade here if we pass the end or the start of
        // the track in reverse direction
        // because we assume that the track samples itself start and stop
        // towards zero.
        // If it turns out that ramping is required be aware that the end
        // or start may pass in the middle of the buffer.
    } else {
        // Pause
        if (m_bCrossfadeReady) {
            // We don't ramp here, since EnginePregain handles fades
            // from and to speed == 0
            SampleUtil::copy(pOutput, m_pCrossfadeBuffer, iBufferSize);
        } else {
            SampleUtil::clear(pOutput, iBufferSize);
        }
    }

    const auto currentPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_filepos_play);
    const auto trackEndPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_trackSamplesOld);
    for (const auto& pControl: qAsConst(m_engineControls)) {
        pControl->setFrameInfo(currentPosition, trackEndPosition, m_trackSampleRateOld);
        pControl->process(rate, currentPosition, iBufferSize);
    }

    m_scratching_old = is_scratching;

    // Handle repeat mode
    bool at_start = m_filepos_play <= 0;
    at_end = m_filepos_play >= m_trackSamplesOld;

    bool repeat_enabled = m_pRepeat->toBool();

    bool end_of_track = //(at_start && backwards) ||
            (at_end && !backwards);

    // If playbutton is pressed, check if we are at start or end of track
    if ((m_playButton->toBool() || (m_fwdButton->toBool() || m_backButton->toBool()))
            && end_of_track) {
        if (repeat_enabled) {
            double fractionalPos = at_start ? 1.0 : 0;
            doSeekFractional(fractionalPos, SEEK_STANDARD);
        } else {
            m_playButton->set(0.);
        }
    }

    // Give the Reader hints as to which chunks of the current song we
    // really care about. It will try very hard to keep these in memory
    hintReader(rate);
}

void EngineBuffer::process(CSAMPLE* pOutput, const int iBufferSize) {
    // Bail if we receive a buffer size with incomplete sample frames. Assert in debug builds.
    VERIFY_OR_DEBUG_ASSERT((iBufferSize % kSamplesPerFrame) == 0) {
        return;
    }
    m_pReader->process();
    // Steps:
    // - Lookup new reader information
    // - Calculate current rate
    // - Scale the audio with m_pScale, copy the resulting samples into the
    //   output buffer
    // - Give EngineControl's a chance to do work / request seeks, etc
    // - Process repeat mode if we're at the end or beginning of a track
    // - Set last sample value (m_fLastSampleValue) so that rampOut works? Other
    //   miscellaneous upkeep issues.

    m_sampleRate = mixxx::audio::SampleRate::fromDouble(m_pSampleRate->get());

    // If the sample rate has changed, force Rubberband to reset so that
    // it doesn't reallocate when the user engages keylock during playback.
    // We do this even if rubberband is not active.
    m_pScaleLinear->setSampleRate(m_sampleRate);
    m_pScaleST->setSampleRate(m_sampleRate);
    m_pScaleRB->setSampleRate(m_sampleRate);

    bool bTrackLoading = atomicLoadRelaxed(m_iTrackLoading) != 0;
    if (!bTrackLoading && m_pause.tryLock()) {
        processTrackLocked(pOutput, iBufferSize, m_sampleRate);
        // release the pauselock
        m_pause.unlock();
    } else {
        // We are loading a new Track

        // Here the old track was playing and loading the new track is in
        // progress. We can't predict when it happens, so we are not able
        // to collect old samples. New samples are also not in place and
        // we can't predict when they will be in place.
        // If one does this, a click from breaking the last track is somehow
        // natural and he should know that such sound should not be played to
        // the master (audience).
        // Workaround: Simply pause the track before.

        // TODO(XXX):
        // A click free solution requires more refactoring how loading a track
        // is handled. For now we apply a rectangular Gain change here which
        // may click.

        SampleUtil::clear(pOutput, iBufferSize);

        m_rate_old = 0;
        m_speed_old = 0;
        m_scratching_old = false;
    }

#ifdef __SCALER_DEBUG__
    for (int i=0; i<iBufferSize; i+=2) {
        writer << pOutput[i] << "\n";
    }
#endif

    if (isLeader(m_pSyncControl->getSyncMode())) {
        // Report our speed to SyncControl immediately instead of waiting
        // for postProcess so we can broadcast this update to followers.
        m_pSyncControl->reportPlayerSpeed(m_speed_old, m_scratching_old);
    }

    m_iLastBufferSize = iBufferSize;
    m_bCrossfadeReady = false;
}

void EngineBuffer::processSlip(int iBufferSize) {
    // Do a single read from m_bSlipEnabled so we don't run in to race conditions.
    bool enabled = m_pSlipButton->toBool();
    if (enabled != m_bSlipEnabledProcessing) {
        m_bSlipEnabledProcessing = enabled;
        if (enabled) {
            m_dSlipPosition = m_filepos_play;
            m_dSlipRate = m_rate_old;
        } else {
            // TODO(owen) assuming that looping will get canceled properly
            double newPlayFrame = m_dSlipPosition / kSamplesPerFrame;
            double roundedSlip = round(newPlayFrame) * kSamplesPerFrame;
            slotControlSeekExact(roundedSlip);
            m_dSlipPosition = 0;
        }
    }

    // Increment slip position even if it was just toggled -- this ensures the position is correct.
    if (enabled) {
        m_dSlipPosition += static_cast<double>(iBufferSize) * m_dSlipRate;
    }
}

void EngineBuffer::processSyncRequests() {
    SyncRequestQueued enable_request =
            static_cast<SyncRequestQueued>(
                    m_iEnableSyncQueued.fetchAndStoreRelease(SYNC_REQUEST_NONE));
    SyncMode mode_request =
            static_cast<SyncMode>(m_iSyncModeQueued.fetchAndStoreRelease(
                    static_cast<int>(SyncMode::Invalid)));
    switch (enable_request) {
    case SYNC_REQUEST_ENABLE:
        m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::Follower);
        break;
    case SYNC_REQUEST_DISABLE:
        m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::None);
        break;
    case SYNC_REQUEST_ENABLEDISABLE:
        m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::Follower);
        m_pEngineSync->requestSyncMode(m_pSyncControl, SyncMode::None);
        break;
    case SYNC_REQUEST_NONE:
        break;
    }
    if (mode_request != SyncMode::Invalid) {
        m_pEngineSync->requestSyncMode(m_pSyncControl,
                static_cast<SyncMode>(mode_request));
    }
}

void EngineBuffer::processSeek(bool paused) {
    m_previousBufferSeek = false;
    // Check if we are cloning another channel before doing any seeking.
    EngineChannel* pChannel = m_pChannelToCloneFrom.fetchAndStoreRelaxed(nullptr);
    if (pChannel) {
        seekCloneBuffer(pChannel->getEngineBuffer());
    }

    const QueuedSeek queuedSeek = m_queuedSeek.getValue();

    SeekRequests seekType = queuedSeek.seekType;
    mixxx::audio::FramePos position = queuedSeek.position;

    // Add SEEK_PHASE bit, if any
    if (m_iSeekPhaseQueued.fetchAndStoreRelease(0)) {
        seekType |= SEEK_PHASE;
    }

    switch (seekType) {
        case SEEK_NONE:
            return;
        case SEEK_PHASE:
            // only adjust phase
            position = mixxx::audio::FramePos::fromEngineSamplePos(m_filepos_play);
            break;
        case SEEK_STANDARD:
            if (m_pQuantize->toBool()) {
                seekType |= SEEK_PHASE;
            }
            // new position was already set above
            break;
        case SEEK_EXACT:
        case SEEK_EXACT_PHASE: // artificial state = SEEK_EXACT | SEEK_PHASE
        case SEEK_STANDARD_PHASE: // artificial state = SEEK_STANDARD | SEEK_PHASE
            // new position was already set above
            break;
        default:
            DEBUG_ASSERT(!"Unhandled seek request type");
            m_queuedSeek.setValue(kNoQueuedSeek);
            return;
    }

    VERIFY_OR_DEBUG_ASSERT(position.isValid()) {
        return;
    }

    const auto trackEndPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_trackSamplesOld);

    // Don't allow the playposition to go past the end.
    if (position > trackEndPosition) {
        position = trackEndPosition;
    }

    if (!paused && (seekType & SEEK_PHASE)) {
        if (kLogger.traceEnabled()) {
            kLogger.trace() << "EngineBuffer::processSeek" << getGroup() << "Seeking phase";
        }
        const mixxx::audio::FramePos syncPosition =
                m_pBpmControl->getBeatMatchPosition(position, true, true);
        position = m_pLoopingControl->getSyncPositionInsideLoop(position, syncPosition);
        if (kLogger.traceEnabled()) {
            kLogger.trace()
                    << "EngineBuffer::processSeek" << getGroup() << "seek info:" << m_filepos_play
                    << "->" << position;
        }
    }
    if (position.toEngineSamplePos() != m_filepos_play) {
        if (kLogger.traceEnabled()) {
            kLogger.trace() << "EngineBuffer::processSeek" << getGroup() << "Seek to" << position;
        }
        setNewPlaypos(position.toEngineSamplePos());
        m_previousBufferSeek = true;
    }
    // Reset the m_queuedSeek value after it has been processed in
    // setNewPlaypos() so that the Engine Controls have always access to the
    // position of the upcoming buffer cycle (used for loop cues)
    m_queuedSeek.setValue(kNoQueuedSeek);
}

void EngineBuffer::postProcess(const int iBufferSize) {
    // The order of events here is very delicate.  It's necessary to update
    // some values before others, because the later updates may require
    // values from the first update.
    if (kLogger.traceEnabled()) {
        kLogger.trace() << getGroup() << "EngineBuffer::postProcess";
    }
    const mixxx::Bpm localBpm = m_pBpmControl->updateLocalBpm();
    double beatDistance = m_pBpmControl->updateBeatDistance();
    // FIXME: Double check if calling setLocalBpm with an invalid value is correct and intended.
    mixxx::Bpm newLocalBpm;
    if (localBpm.isValid()) {
        newLocalBpm = localBpm;
    }
    m_pSyncControl->setLocalBpm(newLocalBpm);
    m_pSyncControl->updateAudible();
    SyncMode mode = m_pSyncControl->getSyncMode();
    if (isLeader(mode)) {
        m_pEngineSync->notifyBeatDistanceChanged(m_pSyncControl, beatDistance);
    } else if (isFollower(mode)) {
        // Report our speed to SyncControl.  If we are leader, we already did this.
        m_pSyncControl->reportPlayerSpeed(m_speed_old, m_scratching_old);
        m_pSyncControl->updateTargetBeatDistance();
    }

    // Update all the indicators that EngineBuffer publishes to allow
    // external parts of Mixxx to observe its status.
    updateIndicators(m_speed_old, iBufferSize);
}

mixxx::audio::FramePos EngineBuffer::queuedSeekPosition() const {
    const QueuedSeek queuedSeek = m_queuedSeek.getValue();
    if (queuedSeek.seekType == SEEK_NONE) {
        return {};
    }

    return queuedSeek.position;
}

void EngineBuffer::updateIndicators(double speed, int iBufferSize) {
    if (!m_trackSampleRateOld.isValid()) {
        // This happens if Deck Passthrough is active but no track is loaded.
        // We skip indicator updates.
        return;
    }

    // Increase samplesCalculated by the buffer size
    m_iSamplesSinceLastIndicatorUpdate += iBufferSize;

    const double fFractionalPlaypos = fractionalPlayposFromAbsolute(m_filepos_play);

    const double tempoTrackSeconds = m_trackSamplesOld / kSamplesPerFrame
            / m_trackSampleRateOld / m_tempo_ratio_old;
    if(speed > 0 && fFractionalPlaypos == 1.0) {
        // At Track end
        speed = 0;
    }

    // Report fractional playpos to SyncControl.
    // TODO(rryan) It's kind of hacky that this is in updateIndicators but it
    // prevents us from computing fFractionalPlaypos multiple times per
    // EngineBuffer::process().
    m_pSyncControl->reportTrackPosition(fFractionalPlaypos);

    // Update indicators that are only updated after every
    // sampleRate/kiUpdateRate samples processed.  (e.g. playposSlider)
    if (m_iSamplesSinceLastIndicatorUpdate >
            (kSamplesPerFrame * m_pSampleRate->get() /
                    kPlaypositionUpdateRate)) {
        m_playposSlider->set(fFractionalPlaypos);
        m_pCueControl->updateIndicators();
    }

    // Update visual control object, this needs to be done more often than the
    // playpos slider
    m_visualPlayPos->set(fFractionalPlaypos, speed * m_baserate_old,
            (double)iBufferSize / m_trackSamplesOld,
            fractionalPlayposFromAbsolute(m_dSlipPosition),
            tempoTrackSeconds);

    // TODO: Especially with long audio buffers, jitter is visible. This can be fixed by moving the
    // ClockControl::updateIndicators into the waveform update loop which is synced with the display refresh rate.
    // Via the visual play position it's possible to access to the sample that is currently played,
    // and not the one that have been processed as in the current solution.
    const auto currentPosition = mixxx::audio::FramePos::fromEngineSamplePos(m_filepos_play);
    const auto sampleRate = mixxx::audio::SampleRate::fromDouble(m_pSampleRate->get());
    m_pClockControl->updateIndicators(speed * m_baserate_old, currentPosition, sampleRate);
}

void EngineBuffer::hintReader(const double dRate) {
    m_hintList.clear();
    m_pReadAheadManager->hintReader(dRate, &m_hintList);

    //if slipping, hint about virtual position so we're ready for it
    if (m_bSlipEnabledProcessing) {
        Hint hint;
        hint.frame = SampleUtil::floorPlayPosToFrame(m_dSlipPosition);
        hint.priority = 1;
        if (m_dSlipRate >= 0) {
            hint.frameCount = Hint::kFrameCountForward;
        } else {
            hint.frameCount = Hint::kFrameCountBackward;
        }
        m_hintList.append(hint);
    }

    for (const auto& pControl: qAsConst(m_engineControls)) {
        pControl->hintReader(&m_hintList);
    }
    m_pReader->hintAndMaybeWake(m_hintList);
}

// WARNING: This method runs in the GUI thread
void EngineBuffer::loadTrack(TrackPointer pTrack, bool play) {
    if (pTrack) {
        // Signal to the reader to load the track. The reader will respond with
        // trackLoading and then either with trackLoaded or trackLoadFailed signals.
        m_bPlayAfterLoading = play;
        m_pReader->newTrack(pTrack);
    } else {
        // Loading a null track means "eject"
        ejectTrack();
    }
}

void EngineBuffer::addControl(EngineControl* pControl) {
    // Connect to signals from EngineControl here...
    m_engineControls.push_back(pControl);
    pControl->setEngineBuffer(this);
}

bool EngineBuffer::isTrackLoaded() const {
    if (m_pCurrentTrack) {
        return true;
    }
    return false;
}

TrackPointer EngineBuffer::getLoadedTrack() const {
    return m_pCurrentTrack;
}

void EngineBuffer::slotEjectTrack(double v) {
    if (v > 0) {
        // Don't allow rejections while playing a track. We don't need to lock to
        // call ControlObject::get() so this is fine.
        if (m_playButton->get() > 0) {
            return;
        }
        ejectTrack();
    }
}

double EngineBuffer::getExactPlayPos() const {
    if (!m_visualPlayPos->isValid()) {
        return 0.0;
    }
    return m_visualPlayPos->getEnginePlayPos() * getTrackSamples();
}

double EngineBuffer::getVisualPlayPos() const {
    return m_visualPlayPos->getEnginePlayPos();
}

double EngineBuffer::getTrackSamples() const {
    return m_pTrackSamples->get();
}

double EngineBuffer::getUserOffset() const {
    return m_pBpmControl->getUserOffset();
}

double EngineBuffer::getRateRatio() const {
    if (m_pBpmControl != nullptr) {
        return m_pBpmControl->getRateRatio();
    }
    return 1.0;
}

void EngineBuffer::collectFeatures(GroupFeatureState* pGroupFeatures) const {
    if (m_pBpmControl != nullptr) {
        m_pBpmControl->collectFeatures(pGroupFeatures);
    }
}

void EngineBuffer::slotUpdatedTrackBeats() {
    TrackPointer pTrack = m_pCurrentTrack;
    if (pTrack) {
        for (const auto& pControl : qAsConst(m_engineControls)) {
            pControl->trackBeatsUpdated(pTrack->getBeats());
        }
    }
}

void EngineBuffer::setScalerForTest(
        EngineBufferScale* pScaleVinyl,
        EngineBufferScale* pScaleKeylock) {
    m_pScaleVinyl = pScaleVinyl;
    m_pScaleKeylock = pScaleKeylock;
    m_pScale = m_pScaleVinyl;
    m_pScale->clear();
    m_bScalerChanged = true;
    // This bool is permanently set and can't be undone.
    m_bScalerOverride = true;
}
