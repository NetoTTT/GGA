/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "CoreController.h"

#include "ConfigController.h"
#include "InputController.h"
#include "LogController.h"
#include "MemoryAccessLogController.h"
#include "MultiplayerController.h"
#include "Override.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QFile>
#include <QMessageBox>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

#include <chrono>

#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#include <mgba/feature/video-logger.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/renderers/cache-set.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba/internal/gba/sharkport.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/renderers/cache-set.h>
#endif
#include "feature/sqlite3/no-intro.h"
#include <mgba-util/math.h>
#include <mgba-util/vfs.h>

#include "moc_CoreController.cpp"

#define AUTOSAVE_GRANULARITY 600

using namespace QGBA;

CoreController::CoreController(mCore* core, QObject* parent)
	: QObject(parent)
	, m_loadStateFlags(SAVESTATE_SCREENSHOT | SAVESTATE_RTC)
	, m_saveStateFlags(SAVESTATE_SCREENSHOT | SAVESTATE_SAVEDATA | SAVESTATE_CHEATS | SAVESTATE_RTC)
{
	m_threadContext.core = core;
	m_threadContext.userData = this;
	updateROMInfo();

#ifdef M_CORE_GBA
	GBASIODolphinCreate(&m_dolphin);
#endif

#ifdef ENABLE_DEBUGGERS
	mDebuggerInit(&m_debugger);
#endif

	m_resetActions.append([this]() {
		if (m_autoload) {
			mCoreLoadState(m_threadContext.core, 0, m_loadStateFlags);
		}
	});

	m_threadContext.startCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);

		switch (context->core->platform(context->core)) {
#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			context->core->setPeripheral(context->core, mPERIPH_GBA_LUMINANCE, controller->m_inputController->luminance());
			break;
#endif
		default:
			break;
		}

		controller->updateFastForward();

		if (controller->m_multiplayer) {
			controller->m_multiplayer->attachGame(controller);
			controller->updatePlayerSave();
		}

		if (controller->m_override) {
			controller->m_override->identify(context->core);
			context->core->setOverride(context->core, controller->m_override->raw());
		}

		QMetaObject::invokeMethod(controller, "started");
	};

	m_threadContext.resetCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		for (auto& action : controller->m_resetActions) {
			action();
		}

		controller->m_resetActions.clear();
		controller->m_frameCounter = -1;

		if (!controller->m_hwaccel) {
			context->core->setVideoBuffer(context->core, reinterpret_cast<mColor*>(controller->m_activeBuffer.data()), controller->screenDimensions().width());
		}

		QString message(tr("Reset r%1-%2 %3").arg(gitRevision).arg(QLatin1String(gitCommitShort)).arg(controller->m_crc32, 8, 16, QLatin1Char('0')));
		QMetaObject::invokeMethod(controller, "didReset");
		if (controller->m_showResetInfo) {
			QMetaObject::invokeMethod(controller, "statusPosted", Q_ARG(const QString&, message));
		}
		controller->finishFrame();
	};

	m_threadContext.frameCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);

		if (controller->m_autosaveCounter == AUTOSAVE_GRANULARITY) {
			if (controller->m_autosave) {
				mCoreSaveState(context->core, 0, controller->m_saveStateFlags);
			}
			controller->m_autosaveCounter = 0;
		}
		++controller->m_autosaveCounter;

		controller->finishFrame();
	};

	m_threadContext.cleanCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);

		if (controller->m_autosave) {
			mCoreSaveState(context->core, 0, controller->m_saveStateFlags);
		}

		controller->clearMultiplayerController();
#ifdef M_CORE_GBA
		controller->detachDolphin();
#endif
		QMetaObject::invokeMethod(controller, "stopping");
	};

	m_threadContext.pauseCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);

#ifdef M_CORE_GBA
		// v2 pauses the core thread itself to hand it to the runner; that
		// is not a user pause and must not reach the UI.
		if (controller->m_lan2Running) {
			return;
		}
#endif
		QMetaObject::invokeMethod(controller, "paused");
	};

	m_threadContext.unpauseCallback = [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);

#ifdef M_CORE_GBA
		if (controller->m_lan2Running) {
			return;
		}
#endif
		QMetaObject::invokeMethod(controller, "unpaused");
	};

	m_logger.self = this;
	m_logger.log = [](mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
		CoreLogger* logContext = static_cast<CoreLogger*>(logger);

		static const char* savestateMessage = "State %i saved";
		static const char* loadstateMessage = "State %i loaded";
		static const char* savestateFailedMessage = "State %i failed to load";
		static int biosCat = -1;
		static int statusCat = -1;
		if (!logContext) {
			return;
		}
		CoreController* controller = logContext->self;
		QString message;
		if (biosCat < 0) {
			biosCat = mLogCategoryById("gba.bios");
		}
		if (statusCat < 0) {
			statusCat = mLogCategoryById("core.status");
		}
#ifdef M_CORE_GBA
		if (level == mLOG_STUB && category == biosCat) {
			va_list argc;
			va_copy(argc, args);
			int immediate = va_arg(argc, int);
			va_end(argc);
			QMetaObject::invokeMethod(controller, "unimplementedBiosCall", Q_ARG(int, immediate));
		} else
#endif
		if (category == statusCat) {
			// Slot 0 is reserved for suspend points
			if (strncmp(loadstateMessage, format, strlen(loadstateMessage)) == 0) {
				va_list argc;
				va_copy(argc, args);
				int slot = va_arg(argc, int);
				va_end(argc);
				if (slot == 0) {
					format = "Loaded suspend state";
				}
			} else if (strncmp(savestateFailedMessage, format, strlen(savestateFailedMessage)) == 0 || strncmp(savestateMessage, format, strlen(savestateMessage)) == 0) {
				va_list argc;
				va_copy(argc, args);
				int slot = va_arg(argc, int);
				va_end(argc);
				if (slot == 0) {
					return;
				}
			}
			va_list argc;
			va_copy(argc, args);
			message = QString::vasprintf(format, argc);
			va_end(argc);
			QMetaObject::invokeMethod(controller, "statusPosted", Q_ARG(const QString&, message));
		}
		message = QString::vasprintf(format, args);
		QMetaObject::invokeMethod(controller, "logPosted", Q_ARG(int, level), Q_ARG(int, category), Q_ARG(const QString&, message));
		if (level == mLOG_FATAL && !controller->m_crashSeen) {
			controller->m_crashSeen = true;
			QMetaObject::invokeMethod(controller, "crashed", Q_ARG(const QString&, message));
		}
	};
	m_threadContext.logger.logger = &m_logger;
}

CoreController::~CoreController() {
	endVideoLog();
	stop();
	disconnect();
	detachLAN();

	mCoreThreadJoin(&m_threadContext);

#ifdef ENABLE_DEBUGGERS
	mDebuggerDeinit(&m_debugger);
#endif

	if (m_cacheSet) {
		mCacheSetDeinit(m_cacheSet.get());
		m_cacheSet.reset();
	}

	mCoreConfigDeinit(&m_threadContext.core->config);
	m_threadContext.core->deinit(m_threadContext.core);
}

void CoreController::setPath(const QString& path, const QString& base) {
	m_path = path;
	m_baseDirectory = base;
}

const mColor* CoreController::drawContext() {
	if (m_hwaccel) {
		return nullptr;
	}
	QMutexLocker locker(&m_bufferMutex);
	return reinterpret_cast<const mColor*>(m_completeBuffer.constData());
}

QImage CoreController::getPixels() {
	QByteArray buffer;
	QSize size = screenDimensions();
	size_t stride = size.width() * BYTES_PER_PIXEL;

	if (!m_hwaccel) {
		buffer = m_completeBuffer;
	} else {
		Interrupter interrupter(this);
		const void* pixels;
		m_threadContext.core->getPixels(m_threadContext.core, &pixels, &stride);
		stride *= BYTES_PER_PIXEL;
		buffer = QByteArray::fromRawData(static_cast<const char*>(pixels), stride * size.height());
	}

	QImage image(reinterpret_cast<const uchar*>(buffer.constData()),
	             size.width(), size.height(), stride, QImage::Format_RGBX8888);
	image.bits(); // Cause QImage to detach
	return image;
}

bool CoreController::isPaused() {
#ifdef M_CORE_GBA
	// In v2 the core thread is always paused (the runner steps the core),
	// so the user-visible pause state is tracked separately.
	if (m_lan2Running) {
		return m_lan2Paused;
	}
#endif
	return mCoreThreadIsPaused(&m_threadContext);
}

bool CoreController::hasStarted() {
	return mCoreThreadHasStarted(&m_threadContext);
}

mPlatform CoreController::platform() const {
	return m_threadContext.core->platform(m_threadContext.core);
}

QSize CoreController::screenDimensions() const {
	unsigned width, height;
	m_threadContext.core->currentVideoSize(m_threadContext.core, &width, &height);

	return QSize(width, height);
}

unsigned CoreController::videoScale() const {
	return m_threadContext.core->videoScale(m_threadContext.core);
}

void CoreController::loadConfig(ConfigController* config) {
	Interrupter interrupter(this);
	m_loadStateFlags = config->getOption("loadStateExtdata", m_loadStateFlags).toInt();
	m_saveStateFlags = config->getOption("saveStateExtdata", m_saveStateFlags).toInt();
	m_fastForwardRatio = config->getOption("fastForwardRatio", m_fastForwardRatio).toFloat();
	m_fastForwardHeldRatio = config->getOption("fastForwardHeldRatio", m_fastForwardRatio).toFloat();
	m_videoSync = config->getOption("videoSync", m_videoSync).toInt();
	m_audioSync = config->getOption("audioSync", m_audioSync).toInt();
	m_fpsTarget = config->getOption("fpsTarget").toFloat();
	m_autosave = config->getOption("autosave", false).toInt();
	m_autoload = config->getOption("autoload", true).toInt();
	m_autofireThreshold = config->getOption("autofireThreshold", m_autofireThreshold).toInt();
	m_fastForwardVolume = config->getOption("fastForwardVolume", -1).toInt();
	m_fastForwardMute = config->getOption("fastForwardMute", false).toInt();
	mCoreConfigCopyValue(&m_threadContext.core->config, config->config(), "volume");
	mCoreConfigCopyValue(&m_threadContext.core->config, config->config(), "mute");
	m_preload = config->getOption("preload", true).toInt();

	QSize sizeBefore = screenDimensions();
	m_activeBuffer.resize(256 * 224 * sizeof(mColor));
	m_threadContext.core->setVideoBuffer(m_threadContext.core, reinterpret_cast<mColor*>(m_activeBuffer.data()), sizeBefore.width());

	mCoreLoadForeignConfig(m_threadContext.core, config->config());

	QSize sizeAfter = screenDimensions();
	m_activeBuffer.resize(sizeAfter.width() * sizeAfter.height() * sizeof(mColor));
	m_threadContext.core->setVideoBuffer(m_threadContext.core, reinterpret_cast<mColor*>(m_activeBuffer.data()), sizeAfter.width());

	if (hasStarted()) {
		updateFastForward();
		mCoreThreadRewindParamsChanged(&m_threadContext);
	}
#ifdef M_CORE_GB
	if (sizeBefore != sizeAfter) {
		mCoreConfigSetIntValue(&m_threadContext.core->config, "sgb.borders", 0);
		m_threadContext.core->reloadConfigOption(m_threadContext.core, "sgb.borders", nullptr);
		mCoreConfigCopyValue(&m_threadContext.core->config, config->config(), "sgb.borders");
		m_threadContext.core->reloadConfigOption(m_threadContext.core, "sgb.borders", nullptr);
	}
	m_threadContext.core->reloadConfigOption(m_threadContext.core, "gb.pal", config->config());
#endif
}

#ifdef ENABLE_DEBUGGERS
void CoreController::attachDebugger(bool interrupt) {
	Interrupter interrupter(this);
	if (!m_threadContext.core->debugger) {
		mDebuggerAttach(&m_debugger, m_threadContext.core);
	}
	if (interrupt) {
		mDebuggerEnter(&m_debugger, DEBUGGER_ENTER_ATTACHED, 0);
	}
}

void CoreController::detachDebugger() {
	Interrupter interrupter(this);
	if (!m_threadContext.core->debugger) {
		return;
	}
	m_threadContext.core->detachDebugger(m_threadContext.core);
}

void CoreController::attachDebuggerModule(mDebuggerModule* module, bool interrupt) {
	Interrupter interrupter(this);
	if (module) {
		mDebuggerAttachModule(&m_debugger, module);
	}
	attachDebugger(interrupt);
}

void CoreController::detachDebuggerModule(mDebuggerModule* module) {
	Interrupter interrupter(this);
	mDebuggerDetachModule(&m_debugger, module);
}
#endif

void CoreController::setMultiplayerController(MultiplayerController* controller) {
	if (controller == m_multiplayer) {
		return;
	}
	clearMultiplayerController();
	m_multiplayer = controller;
	if (!mCoreThreadHasStarted(&m_threadContext)) {
		return;
	}
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* thread) {
		CoreController* controller = static_cast<CoreController*>(thread->userData);
		controller->m_multiplayer->attachGame(controller);
	});
}

void CoreController::clearMultiplayerController() {
	if (!m_multiplayer) {
		return;
	}
	m_multiplayer->detachGame(this);
	m_multiplayer = nullptr;
}

mCacheSet* CoreController::graphicCaches() {
	if (m_cacheSet) {
		return m_cacheSet.get();
	}
	Interrupter interrupter(this);
	switch (platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		GBA* gba = static_cast<GBA*>(m_threadContext.core->board);
		m_cacheSet = std::make_unique<mCacheSet>();
		GBAVideoCacheInit(m_cacheSet.get());
		GBAVideoCacheAssociate(m_cacheSet.get(), &gba->video);
		break;
	}
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB: {
		GB* gb = static_cast<GB*>(m_threadContext.core->board);
		m_cacheSet = std::make_unique<mCacheSet>();
		GBVideoCacheInit(m_cacheSet.get());
		GBVideoCacheAssociate(m_cacheSet.get(), &gb->video);
		break;
	}
#endif
	default:
		return nullptr;
	}
	return m_cacheSet.get();
}

#ifdef M_CORE_GBA
bool CoreController::attachDolphin(const Address& address) {
	if (platform() != mPLATFORM_GBA) {
		return false;
	}
	if (GBASIODolphinConnect(&m_dolphin, &address, 0, 0)) {
		clearMultiplayerController();
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, &m_dolphin.d);
		return true;
	}
	return false;
}

void CoreController::detachDolphin() {
	if (platform() == mPLATFORM_GBA) {
		// TODO: Reattach to multiplayer controller
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
	}
	GBASIODolphinDestroy(&m_dolphin);
}

static int64_t lanNowMs(void*) {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* v2 netplay diagnostics are opt-in: the probes read both cores next to the
 * scheduling they measure, so a normal run must not pay for them. */
static bool lanDiagEnabled() {
	static int enabled = -1;
	if (enabled < 0) {
		enabled = qgetenv("GGA_DIAG").isEmpty() ? 0 : 1;
	}
	return enabled;
}

#ifdef M_CORE_GBA
static CoreController* lanOwner(void* context) {
	auto* ep = static_cast<GBASIONetEndpoint*>(context);
	return ep ? static_cast<CoreController*>(ep->userdata) : nullptr;
}

static GBA* lanGBA(CoreController* self) {
	if (!self || self->platform() != mPLATFORM_GBA) {
		return nullptr;
	}
	return static_cast<GBA*>(self->thread()->core->board);
}

time_t CoreController::transportRtcUnixTime(struct mRTCSource* source) {
	auto* rtc = reinterpret_cast<LANRtcSource*>(source);
	int64_t elapsed = lanNowMs(nullptr) - rtc->installedMs;
	return static_cast<time_t>((rtc->baseUnixMs + elapsed) / 1000);
}

void CoreController::transportGetRtcSeed(void* context, int64_t* epochMs) {
	CoreController* self = lanOwner(context);
	*epochMs = self ? self->m_lanRtc.baseUnixMs : 0;
}

void CoreController::transportLoadRtcSeed(void* context, int64_t epochMs) {
	CoreController* self = lanOwner(context);
	if (self) {
		self->installLANRtc(epochMs);
	}
}

void CoreController::installLANRtc(int64_t epochMs) {
	GBA* gba = lanGBA(this);
	if (!gba) {
		return;
	}
	Interrupter interrupter(this);
	m_lanRtc.baseUnixMs = epochMs;
	m_lanRtc.installedMs = lanNowMs(nullptr);
	m_lanRtc.sample = nullptr;
	m_lanRtc.unixTime = &CoreController::transportRtcUnixTime;
	m_lanRtc.serialize = nullptr;
	m_lanRtc.deserialize = nullptr;
	gba->memory.hw.rtc.offset = 0;
	gba->rtcSource = &m_lanRtc;
}
#endif

void CoreController::pumpLAN() {
#ifdef M_CORE_GBA
	if (m_lan2Active) {
		pumpLAN2();
		return;
	}
	if (m_lanActive) {
		GBASIONetEndpointPoll(&m_lanEndpoint);
	}
#endif
}

bool CoreController::attachLAN(bool isHost, const QString& address, int port) {
	if (platform() != mPLATFORM_GBA) {
		return false;
	}
	if (m_lanActive) {
		detachLAN();
	}
	m_lanError.clear();
	m_lanJoinStartMs = 0;
	// Mirror stderr to a per-process file: GUI builds have no console,
	// and the netplay traces are the only window into link issues.
	QString logPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ QStringLiteral("/lanplay-%1.log").arg(QCoreApplication::applicationPid());
	if (freopen(logPath.toLocal8Bit().constData(), "w", stderr)) {
		setvbuf(stderr, nullptr, _IONBF, 0);
	}
	// ROM identity must match the peer: FNV-1a over the first 64 bytes,
	// same layout as the headless link harness. Read from the loaded
	// ROM image so archives (.zip) hash identically to flat dumps.
	uint8_t romHash[32] = {};
	GBA* gba = static_cast<GBA*>(m_threadContext.core->board);
	size_t romAvail = gba ? gba->memory.romSize : 0;
	const uint8_t* romBytes = gba ? reinterpret_cast<const uint8_t*>(gba->memory.rom) : nullptr;
	QByteArray head;
	if (romBytes && romAvail >= 64) {
		head = QByteArray(reinterpret_cast<const char*>(romBytes), 64);
	} else {
		QFile rom(path());
		if (rom.open(QIODevice::ReadOnly)) {
			head = rom.read(64);
		}
	}
	if (head.size() >= 64) {
		uint32_t h = 0x811C9DC5u;
		for (int i = 0; i < 64; ++i) {
			h = (h ^ uint8_t(head[i])) * 16777619u;
		}
		romHash[0] = h & 0xFF;
		romHash[1] = (h >> 8) & 0xFF;
		romHash[2] = (h >> 16) & 0xFF;
		romHash[3] = (h >> 24) & 0xFF;
	}
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
	clearMultiplayerController();
	mLockstepThreadUserInit(&m_lanUser, thread());
	m_lanUser.parent = this;
	m_lanUser.d.requestedId = [](mLockstepUser* ctx) {
		auto* tctx = reinterpret_cast<mLockstepThreadUser*>(ctx);
		auto* user = static_cast<LANLockstepUser*>(tctx);
		return user->parent->m_lanLink.assignedSlot;
	};
	GBASIOLockstepCoordinatorInit(&m_lanCoord);
	memset(&m_lanTransport, 0, sizeof(m_lanTransport));
	m_lanTransport.nowMs = lanNowMs;
	m_lanTransport.getRtcSeed = CoreController::transportGetRtcSeed;
	m_lanTransport.loadRtcSeed = CoreController::transportLoadRtcSeed;
	GBASIOLockstepDriverCreate(&m_lanDriver, &m_lanUser.d);
	GBASIONetLinkInit(&m_lanLink, &m_lanCoord, &m_lanDriver, &m_lanUser.d);
	GBASIONetEndpointInit(&m_lanEndpoint, &m_lanLink, &m_lanTransport);
	m_lanEndpoint.userdata = this;
	GBASIONetEndpointSetIdentity(&m_lanEndpoint, romHash, isHost ? "P0" : "P1");
	if (isHost) {
		// Listen-only for now: the game keeps running solo (visible)
		// until a client knocks. pollHostSession() starts the linked
		// session (attach + reset) when the HELLO arrives, so both
		// sides reboot from identical states.
		installLANRtc(QDateTime::currentMSecsSinceEpoch());
		m_lanLink.assignedSlot = 0;
		m_lanLink.isHost = true;
		if (!GBASIONetEndpointListen(&m_lanEndpoint, port)) {
			detachLAN();
			return false;
		}
	} else {
		// Connect only; pollClientAttach() (dialog timer) completes
		// the attach once WELCOME arrives, so the UI never blocks.
		QByteArray hostBytes = address.toUtf8();
		if (!GBASIONetEndpointConnect(&m_lanEndpoint, hostBytes.constData(), port)) {
			detachLAN();
			return false;
		}
		m_lanJoinStartMs = QDateTime::currentMSecsSinceEpoch();
		m_lanError.clear();
	}
	m_lanActive = true;
	startLanWorker();
	return true;
}

void CoreController::startLanWorker() {
#ifdef M_CORE_GBA
	if (m_lanThread) {
		return;
	}
	m_lanThread = new QThread(this);
	QTimer* pump = new QTimer();
	pump->setInterval(5);
	pump->moveToThread(m_lanThread);
	connect(m_lanThread, &QThread::started, pump, QOverload<>::of(&QTimer::start));
	connect(pump, &QTimer::timeout, this, &CoreController::pumpLAN, Qt::DirectConnection);
	connect(m_lanThread, &QThread::finished, pump, &QObject::deleteLater);
	m_lanThread->start();
#endif
}

void CoreController::stopLanWorker() {
#ifdef M_CORE_GBA
	if (!m_lanThread) {
		return;
	}
	m_lanThread->quit();
	m_lanThread->wait();
	delete m_lanThread;
	m_lanThread = nullptr;
#endif
}

bool CoreController::pollClientAttach() {
#ifdef M_CORE_GBA
	if (!m_lanActive || m_lanLink.isHost || m_lanLink.localSlot >= 0) {
		return false;
	}
	if (!m_lanEndpoint.welcomed) {
		if (m_lanJoinStartMs > 0 && QDateTime::currentMSecsSinceEpoch() - m_lanJoinStartMs > 15000) {
			m_lanError = tr("Join timed out waiting for WELCOME.");
			detachLAN();
		}
		return false;
	}
	// Session start, in an order that can never hang:
	//  1. detach any previous link (re-join safe) while paused;
	//  2. unpark (a barrier-parked core ignores reset);
	//  3. resume and reboot on the RUNNING core (reset under pause
	//     never completes: the runloop only honors it while running);
	//  4. attach fresh (both sides reboot within ms of each other,
	//     close enough for barrier sync).
	{
		CoreController::Interrupter interrupter(this);
		if (lanDiagEnabled()) {
			fprintf(stderr, "NET client attach\n");
		}
		if (m_lanDriverAttached) {
			m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
			GBASIOLockstepCoordinatorDetach(&m_lanCoord, &m_lanDriver);
			m_lanDriverAttached = false;
		}
		mCoreThreadStopWaiting(thread());
		interrupter.resume();
	}
	reset();
	if (lanDiagEnabled()) {
		fprintf(stderr, "NET client reset done\n");
	}
	{
		CoreController::Interrupter interrupter(this);
		GBASIOLockstepCoordinatorAttach(&m_lanCoord, &m_lanDriver);
		m_lanDriverAttached = true;
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, &m_lanDriver.d);
		GBASIONetLinkResetClock(&m_lanLink);
		m_lanDriver.d.reset(&m_lanDriver.d);
		GBASIONetLinkAddPeer(&m_lanLink, 0);
		GBASIONetLinkClearStaleSleep(&m_lanLink);
		if (lanDiagEnabled()) {
			fprintf(stderr, "NET client peer added\n");
		}
	interrupter.resume();
	}
	m_lanSession = true;
	return true;
#else
	return false;
#endif
}

void CoreController::detachLAN() {
	if (!m_lanActive) {
		return;
	}
	m_lanActive = false;
	m_lanSession = false;
	stopLanWorker();
	if (platform() == mPLATFORM_GBA) {
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
		GBA* gba = static_cast<GBA*>(m_threadContext.core->board);
		if (gba && gba->rtcSource == &m_lanRtc) {
			Interrupter interrupter(this);
			gba->rtcSource = NULL;
			gba->memory.hw.rtc.offset = 0;
		}
		if (m_lanDriverAttached) {
			GBASIOLockstepCoordinatorDetach(&m_lanCoord, &m_lanDriver);
			m_lanDriverAttached = false;
		}
	}
	GBASIONetLinkDeinit(&m_lanLink);
	GBASIOLockstepCoordinatorDeinit(&m_lanCoord);
	GBASIONetEndpointClose(&m_lanEndpoint);
}

/* v2 deterministic input netplay: both players run in this process under
 * GBASIONetRunner and only key masks cross the wire, so the link is exact by
 * construction instead of relying on in-flight SIO to line up. mCoreThread is
 * paused for the local core (the runner steps it) and resumes on detach. */
time_t CoreController::transportRtcFixed(struct mRTCSource* source) {
	auto* rtc = reinterpret_cast<LAN2RtcSource*>(source);
	return static_cast<time_t>(rtc->epochMs / 1000);
}

void CoreController::transportGetRtcSeed2(void* context, int64_t* epochMs) {
	CoreController* self = lanOwner(context);
	*epochMs = self ? self->m_lan2Rtc.epochMs : 0;
}

void CoreController::transportLoadRtcSeed2(void* context, int64_t epochMs) {
	CoreController* self = lanOwner(context);
	if (self) {
		self->m_lan2Rtc.epochMs = epochMs;
		self->installLAN2Rtc();
	}
}

void CoreController::installLAN2Rtc() {
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	Interrupter interrupter(this);
	m_lan2Rtc.sample = nullptr;
	m_lan2Rtc.unixTime = &CoreController::transportRtcFixed;
	m_lan2Rtc.serialize = nullptr;
	m_lan2Rtc.deserialize = nullptr;
	GBA* local = static_cast<GBA*>(m_threadContext.core->board);
	if (local) {
		local->memory.hw.rtc.offset = 0;
		local->rtcSource = &m_lan2Rtc;
	}
	if (m_lan2Peer) {
		GBA* peer = static_cast<GBA*>(m_lan2Peer->board);
		if (peer) {
			peer->memory.hw.rtc.offset = 0;
			peer->rtcSource = &m_lan2Rtc;
		}
	}
}

bool CoreController::attachLAN2(bool isHost, const QString& address, int port) {
	if (platform() != mPLATFORM_GBA) {
		return false;
	}
	if (m_lanActive) {
		detachLAN();
	}
	if (m_lan2Active) {
		detachLAN2();
	}
	m_lanError.clear();

	// Mirror stderr to a per-process file: this is a GUI build with no
	// console, so the netplay traces are the only window into link issues.
	QString v2LogPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ QStringLiteral("/lanplay-%1.log").arg(QCoreApplication::applicationPid());
	if (freopen(v2LogPath.toLocal8Bit().constData(), "w", stderr)) {
		setvbuf(stderr, nullptr, _IONBF, 0);
	}
	if (lanDiagEnabled()) {
		fprintf(stderr, "DBG v2 attach host=%d port=%d\n", (int) isHost, port);
		fflush(stderr);
	}

	GBA* local = static_cast<GBA*>(m_threadContext.core->board);
	if (!local || !local->memory.rom || !local->memory.romSize) {
		m_lanError = tr("No ROM is loaded.");
		if (lanDiagEnabled()) {
			fprintf(stderr, "DBG v2 attach failed: no rom\n");
		}
		return false;
	}

	// A second GBA built from the image that is actually running here
	// (patches and all), so both players boot byte for byte identical and
	// input-only sync holds. GBASIONetRunnerCreateMirrorCore copies the
	// platform, BIOS, ROM, save, config and peripherals, so the mirror cannot
	// pick up a different setup from the frontend. The peer is stepped by the
	// runner, never by a thread of its own.
	m_lan2PeerBuffer.resize(240 * 160 * sizeof(mColor));
	m_lan2PeerBuffer.fill(0);
	mCore* peer = GBASIONetRunnerCreateMirrorCore(m_threadContext.core,
		reinterpret_cast<mColor*>(m_lan2PeerBuffer.data()), 240);
	if (!peer) {
		m_lanError = tr("Could not mirror the running game into the second core.");
		if (lanDiagEnabled()) {
			fprintf(stderr, "DBG v2 attach failed: peer create\n");
		}
		return false;
	}
	m_lan2Peer = peer;

	uint8_t localSlot = isHost ? 0 : 1;
	uint8_t peerSlot = isHost ? 1 : 0;

	GBASIONetRunnerInit(&m_lan2Runner, 2, &m_lan2State);
	m_lan2RunnerReady = true;
	// The cores are not attached to the runner until the peer shows up:
	// while waiting, the local core keeps playing solo with no link port.
	// (Attaching early would park it on the first transfer with no peer.)
	{
		Interrupter interrupter(this);
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
	}

	GBASIONetSessionInit(&m_lan2State, isHost ? GGA_NET_SESSION_HOST : GGA_NET_SESSION_GUEST,
		localSlot, 2, lanNowMs, this);

	memset(&m_lan2Transport, 0, sizeof(m_lan2Transport));
	m_lan2Transport.nowMs = lanNowMs;
	m_lan2Transport.getRtcSeed = CoreController::transportGetRtcSeed2;
	m_lan2Transport.loadRtcSeed = CoreController::transportLoadRtcSeed2;

	// The link layer is inert in v2 (the session carries the messages), but
	// the endpoint still wants one to talk to.
	GBASIOLockstepCoordinatorInit(&m_lan2LinkCoord);
	GBASIOLockstepDriverCreate(&m_lan2LinkDriver, nullptr);
	GBASIONetLinkInit(&m_lan2Link, &m_lan2LinkCoord, &m_lan2LinkDriver, nullptr);
	m_lan2Link.assignedSlot = localSlot;
	m_lan2Link.isHost = isHost;
	m_lan2Link.localSlot = -1;
	GBASIONetEndpointInit(&m_lan2Endpoint, &m_lan2Link, &m_lan2Transport);
	m_lan2Endpoint.userdata = this;
	// Bind the endpoint to the session: this both gives the session its wire
	// (TICK/INPUT/HASH out) and routes the peer's INPUT/TICK in. Without it
	// the host never ticks and the guest never steps.
	GBASIONetSessionSetPeer(&m_lan2State, &m_lan2Endpoint);

	m_lan2Rtc.epochMs = QDateTime::currentMSecsSinceEpoch();
	installLAN2Rtc();

	m_lan2Host = isHost;
	m_lan2Frame = 0;
	m_lan2Active = true;

	if (isHost) {
		if (!GBASIONetEndpointListen(&m_lan2Endpoint, port)) {
			m_lanError = tr("Could not listen on port %1.").arg(port);
			if (lanDiagEnabled()) {
				fprintf(stderr, "DBG v2 attach failed: listen\n");
			}
			detachLAN2();
			return false;
		}
	} else {
		QByteArray hostBytes = address.toUtf8();
		if (!GBASIONetEndpointConnect(&m_lan2Endpoint, hostBytes.constData(), port)) {
			m_lanError = tr("Could not reach %1:%2.").arg(address).arg(port);
			if (lanDiagEnabled()) {
				fprintf(stderr, "DBG v2 attach failed: connect\n");
			}
			detachLAN2();
			return false;
		}
	}
	startLanWorker();
	if (lanDiagEnabled()) {
		fprintf(stderr, "DBG v2 attach ok\n");
		fflush(stderr);
	}
	return true;
}

void CoreController::startLAN2Session() {
	if (m_lan2Running || !m_lan2Active || !m_lan2Peer) {
		return;
	}
	// The runner takes the local core from here, so pause its thread first:
	// it must not run a transfer while the link is being set up. m_lan2Running
	// is set beforehand so this internal pause never reaches the UI.
	m_lan2Running = true;
	m_lan2Paused = false;
	mCoreThreadPause(&m_threadContext);

	// Both cores reboot together on a clean state, and only now get bound to
	// the runner: attaching earlier would park the core during solo play.
	const QSize size = screenDimensions();
	m_threadContext.core->reset(m_threadContext.core);
	m_lan2Peer->reset(m_lan2Peer);
#ifdef M_CORE_GBA
	/* A reset keeps CPU prefetch latches and the cart save type that the
	 * local core resolved while playing alone, but a freshly created peer
	 * core never saw them. Both cores must start on the same values or the
	 * first state hash checkpoint reports a divergence. */
	if (m_threadContext.core->platform(m_threadContext.core) == mPLATFORM_GBA) {
		struct GBA* local = (struct GBA*) m_threadContext.core->board;
		struct GBA* peer = (struct GBA*) m_lan2Peer->board;
		local->memory.prefetch = false;
		local->memory.biosPrefetch = 0;
		local->memory.lastPrefetchedPc = 0;
		peer->memory.prefetch = false;
		peer->memory.biosPrefetch = 0;
		peer->memory.lastPrefetchedPc = 0;
		/* The cart save type is resolved by the game's own detection writes,
		 * and the two instances may have resolved it at different points
		 * while playing alone. Drop both cores back to autodetect so the
		 * same emulated writes resolve it identically in-session. */
		GBASavedataForceType(&local->memory.savedata, GBA_SAVEDATA_AUTODETECT);
		GBASavedataForceType(&peer->memory.savedata, GBA_SAVEDATA_AUTODETECT);
		/* The peer is built from a default config, so any emulation-relevant
		 * flag the local core derived from the user's config has to be
		 * mirrored or the two copies of a logical core disagree. */
		peer->idleOptimization = local->idleOptimization;
		peer->allowOpposingDirections = local->allowOpposingDirections;
	}
#endif
	m_threadContext.core->setVideoBuffer(m_threadContext.core,
		reinterpret_cast<mColor*>(m_activeBuffer.data()), size.width());
	m_frameCounter = 0;
	uint8_t localSlot = m_lan2Host ? 0 : 1;
	uint8_t peerSlot = m_lan2Host ? 1 : 0;
	if (!GBASIONetRunnerAddCore(&m_lan2Runner, localSlot, m_threadContext.core) ||
			!GBASIONetRunnerAddCore(&m_lan2Runner, peerSlot, m_lan2Peer)) {
		m_lan2Active = false;
		if (lanDiagEnabled()) {
			fprintf(stderr, "DBG v2 addcore failed\n");
		}
		emit statusPosted(tr("Could not attach both cores to the link runner."));
		return;
	}
	if (!m_lan2Host) {
		GBASIONetSessionStart(&m_lan2State);
	}
	m_lan2LastFrameMs = lanNowMs(nullptr);
	if (lanDiagEnabled()) {
		fprintf(stderr, "DBG v2 session start host=%d\n", (int) m_lan2Host);
	}
	emit statusPosted(tr("Link established (v2 input sync)."));
}

void CoreController::pumpLAN2() {
	if (!m_lan2Active) {
		return;
	}
	GBASIONetEndpointPoll(&m_lan2Endpoint);
	if (lanDiagEnabled()) {
		int64_t dnow = lanNowMs(nullptr);
		if (dnow - m_lan2DiagMs >= 1000) {
			m_lan2DiagMs = dnow;
			fprintf(stderr, "DBG v2 state host=%d run=%d paused=%d frame=%u high=%u lag=%d coord=%d wait=%x rf=%llu ended=%d reason=%d\n",
				(int) m_lan2Host, (int) m_lan2Running, (int) m_lan2Paused,
				(unsigned) m_lan2State.frame,
				(unsigned) m_lan2State.guestHigh, (int) m_lan2State.lagging,
				m_lan2Runner.coord.nAttached, m_lan2Runner.coord.waiting,
				(unsigned long long) m_lan2Runner.frames,
				(int) GBASIONetSessionEnded(&m_lan2State), (int) GBASIONetSessionEndReason(&m_lan2State));
			fflush(stderr);
		}
	}
	if (!m_lan2Running) {
		bool ready = m_lan2Host ? m_lan2Endpoint.peerHello : m_lan2Endpoint.welcomed;
		if (ready) {
			if (lanDiagEnabled()) {
				fprintf(stderr, "DBG v2 ready host=%d hello=%d welcomed=%d\n",
					(int) m_lan2Host, (int) m_lan2Endpoint.peerHello, (int) m_lan2Endpoint.welcomed);
			}
			startLAN2Session();
			if (lanDiagEnabled() && m_lan2Running) {
				uint32_t initHash[GGA_NET_MAX_PLAYERS] = { 0 };
				GBASIONetRunnerHashes(&m_lan2Runner, initHash);
				fprintf(stderr, "DBG v2 init host=%d p0=%08X p1=%08X\n",
					(int) m_lan2Host, initHash[0], initHash[1]);
				fflush(stderr);
			}
		}
		return;
	}
	// Pace the simulation to real time; the whole point of v2 is that the
	// network never gates the frame rate.
	if (m_lan2Paused) {
		return;
	}
	int64_t now = lanNowMs(nullptr);
	if (now - m_lan2LastFrameMs < 16) {
		return;
	}
	m_lan2LastFrameMs = now;
	GBASIONetSessionSetLocalInput(&m_lan2State,
		static_cast<uint16_t>(m_lan2LocalKeys));
	bool stepped = GBASIONetRunnerStep(&m_lan2Runner, 0);
	if (stepped) {
		finishFrame();
		++m_lan2Frame;
		if (lanDiagEnabled() && m_lan2Frame % 60 == 1) {
			fprintf(stderr, "DBG v2 frame=%u\n", (unsigned) m_lan2Frame);
		}
	}
	if (GBASIONetSessionEnded(&m_lan2State)) {
		if (!m_lan2HashLogged && lanDiagEnabled()) {
			int s;
			fprintf(stderr, "DBG v2 ended host=%d reason=%d frame=%u\n",
				(int) m_lan2Host, (int) GBASIONetSessionEndReason(&m_lan2State),
				(unsigned) m_lan2State.frame);
			for (s = 0; s < GGA_NET_HASH_WINDOW; ++s) {
				if (!m_lan2State.hashMineValid[s] && !m_lan2State.hashTheirsValid[s]) {
					continue;
				}
				fprintf(stderr, "DBG v2 hash host=%d s=%d f=%u mv=%d tv=%d p0m=%08X p0t=%08X p1m=%08X p1t=%08X\n",
					(int) m_lan2Host, s, (unsigned) m_lan2State.hashFrame[s],
					(int) m_lan2State.hashMineValid[s], (int) m_lan2State.hashTheirsValid[s],
					m_lan2State.hashMine[s][0], m_lan2State.hashTheirs[s][0],
					m_lan2State.hashMine[s][1], m_lan2State.hashTheirs[s][1]);
			}
			fflush(stderr);
		}
		m_lan2HashLogged = true;
		emit statusPosted(tr("Link session ended (%1).").arg(GBASIONetSessionEndReason(&m_lan2State)));
	}
}

void CoreController::lan2DumpState(int slot, const QString& path) {
	if (!m_lan2RunnerReady || slot < 0 || slot >= GGA_NET_MAX_PLAYERS) {
		return;
	}
	Interrupter interrupter(this);
	GBASIONetRunnerDumpNow(&m_lan2Runner, static_cast<uint8_t>(slot),
		path.toLocal8Bit().constData());
}

void CoreController::detachLAN2() {
	if (!m_lan2Active && !m_lan2Peer && !m_lan2RunnerReady) {
		return;
	}
	m_lan2Active = false;
	stopLanWorker();
	if (m_lan2RunnerReady) {
		Interrupter interrupter(this);
		// Unpause while m_lan2Running is still set so the signal stays
		// internal, then hand the core back to mCoreThread.
		mCoreThreadUnpause(&m_threadContext);
		GBASIONetRunnerDeinit(&m_lan2Runner);
		m_lan2RunnerReady = false;
	}
	m_lan2Running = false;
	m_lan2Paused = false;
	// Drop both backlinks before the endpoint goes away: the session keeps a
	// pointer to it, and the endpoint keeps one to the session.
	m_lan2State.peer = NULL;
	m_lan2Endpoint.session = NULL;
	GBASIONetEndpointClose(&m_lan2Endpoint);
	GBASIONetLinkDeinit(&m_lan2Link);
	GBASIOLockstepCoordinatorDeinit(&m_lan2LinkCoord);
	if (m_lan2Peer) {
		GBA* peer = static_cast<GBA*>(m_lan2Peer->board);
		if (peer && peer->rtcSource == &m_lan2Rtc) {
			peer->rtcSource = nullptr;
		}
		m_lan2Peer->setPeripheral(m_lan2Peer, mPERIPH_GBA_LINK_PORT, nullptr);
		mCoreConfigDeinit(&m_lan2Peer->config);
		m_lan2Peer->deinit(m_lan2Peer);
		m_lan2Peer = nullptr;
	}
	GBA* local = static_cast<GBA*>(m_threadContext.core->board);
	if (local && local->rtcSource == &m_lan2Rtc) {
		Interrupter interrupter(this);
		local->rtcSource = nullptr;
		local->memory.hw.rtc.offset = 0;
	}
}


bool CoreController::isLANConnected() const {
#ifdef M_CORE_GBA
	return m_lanActive && !SOCKET_FAILED(m_lanEndpoint.conn);
#else
	return false;
#endif
}

bool CoreController::isLANSessionActive() const {
#ifdef M_CORE_GBA
	return m_lanSession;
#else
	return false;
#endif
}

bool CoreController::pollHostSession() {
#ifdef M_CORE_GBA
	if (!m_lanActive || !m_lanLink.isHost || m_lanSession) {
		return false;
	}
	if (!m_lanEndpoint.peerHello) {
		return false;
	}
	// A client knocked: reboot on the running core, then attach fresh.
	// (Reset under pause never completes; attach-then-reset parks the
	// core where the reset can't land. See pollClientAttach.)
	{
		CoreController::Interrupter interrupter(this);
		if (lanDiagEnabled()) {
			fprintf(stderr, "NET host session start\n");
		}
		if (m_lanDriverAttached) {
			m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, NULL);
			GBASIOLockstepCoordinatorDetach(&m_lanCoord, &m_lanDriver);
			m_lanDriverAttached = false;
		}
		mCoreThreadStopWaiting(thread());
		interrupter.resume();
	}
	reset();
	if (lanDiagEnabled()) {
		fprintf(stderr, "NET host reset done\n");
	}
	{
		CoreController::Interrupter interrupter(this);
		GBASIOLockstepCoordinatorAttach(&m_lanCoord, &m_lanDriver);
		m_lanDriverAttached = true;
		// setPeripheral binds d.p; AddPeer copies it into the proxy, so
		// this must come first (a NULL sio crashes the session later).
		m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, &m_lanDriver.d);
		GBASIONetLinkResetClock(&m_lanLink);
		m_lanDriver.d.reset(&m_lanDriver.d);
		GBASIONetLinkAddPeer(&m_lanLink, 1);
		GBASIONetLinkClearStaleSleep(&m_lanLink);
		if (lanDiagEnabled()) {
			fprintf(stderr, "NET host peer added\n");
		}
		interrupter.resume();
	}
	m_lanSession = true;
	return true;
#else
	return false;
#endif
}

bool CoreController::isLANHosting() const {
#ifdef M_CORE_GBA
	return m_lanActive && m_lanLink.isHost && !SOCKET_FAILED(m_lanEndpoint.listenSock);
#else
	return false;
#endif
}

int CoreController::lanSlot() const {
#ifdef M_CORE_GBA
	return m_lanLink.localSlot;
#else
	return -1;
#endif
}

int CoreController::lanRejectReason() const {
#ifdef M_CORE_GBA
	return m_lanEndpoint.rejectReason;
#else
	return 0;
#endif
}

bool CoreController::lanBarrierPending() const {
#ifdef M_CORE_GBA
	return m_lanLink.barrierPending;
#else
	return false;
#endif
}

int CoreController::lanWaiting() const {
#ifdef M_CORE_GBA
	return m_lanCoord.waiting;
#else
	return 0;
#endif
}

int CoreController::lanAttached() const {
#ifdef M_CORE_GBA
	return m_lanCoord.nAttached;
#else
	return 0;
#endif
}

int CoreController::lanTransferMode() const {
#ifdef M_CORE_GBA
	return m_lanCoord.transferMode;
#else
	return -1;
#endif
}
#endif

void CoreController::setOverride(std::unique_ptr<Override> override) {
	Interrupter interrupter(this);
	m_override = std::move(override);
	m_override->identify(m_threadContext.core);
}

void CoreController::setInputController(InputController* inputController) {
	m_inputController = inputController;
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_ROTATION, m_inputController->rotationSource());
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_RUMBLE, m_inputController->rumble());
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_IMAGE_SOURCE, m_inputController->imageSource());
}

void CoreController::setLogger(LogController* logger) {
	disconnect(m_log);
	m_log = logger;
	m_logger.filter = logger->filter();
	connect(this, &CoreController::logPosted, m_log, &LogController::postLog);
}

#ifdef ENABLE_DEBUGGERS
std::weak_ptr<MemoryAccessLogController> CoreController::memoryAccessLogController() {
	if (!m_malController) {
		m_malController = std::make_shared<MemoryAccessLogController>(this);
	}
	return m_malController;
}
#endif

void CoreController::start() {
	QSize size(screenDimensions());
	m_activeBuffer.resize(size.width() * size.height() * sizeof(mColor));
	m_activeBuffer.fill(0xFF);
	m_completeBuffer = m_activeBuffer;

	m_threadContext.core->setVideoBuffer(m_threadContext.core, reinterpret_cast<mColor*>(m_activeBuffer.data()), size.width());

	if (!m_patched) {
		mCoreAutoloadPatch(m_threadContext.core);
	}
	if (!mCoreThreadStart(&m_threadContext)) {
		emit failed();
		emit stopping();
	}
}

void CoreController::stop() {
#ifdef M_CORE_GBA
	detachLAN2();
#endif
	setSync(false);
#ifdef ENABLE_DEBUGGERS
	if (m_malController) {
		m_malController->stop();
	}

	detachDebugger();
#endif
	setPaused(false);
	mCoreThreadEnd(&m_threadContext);
}

void CoreController::reset() {
	m_crashSeen = false;
	mCoreThreadReset(&m_threadContext);
}

void CoreController::setPaused(bool paused) {
	QMutexLocker locker(&m_actionMutex);
#ifdef M_CORE_GBA
	if (m_lan2Running) {
		// v2 owns the core thread's pause; the button only stops the runner.
		m_lan2Paused = paused;
		QMetaObject::invokeMethod(this, paused ? "paused" : "unpaused");
		return;
	}
#endif
	if (paused) {
		if (m_moreFrames < 0) {
			m_moreFrames = 1;
		}
	} else {
		m_moreFrames = -1;
		if (isPaused()) {
			mCoreThreadUnpause(&m_threadContext);
		}
	}
}

void CoreController::frameAdvance() {
	QMutexLocker locker(&m_actionMutex);
	m_moreFrames = 1;
	if (isPaused()) {
		mCoreThreadUnpause(&m_threadContext);
	}
}

void CoreController::addFrameAction(std::function<void ()> action) {
	QMutexLocker locker(&m_actionMutex);
	m_frameActions.append(action);
}

void CoreController::setSync(bool sync) {
	if (sync) {
		m_threadContext.impl->sync.audioWait = m_audioSync;
		m_threadContext.impl->sync.videoFrameWait = m_videoSync;
	} else {
		m_threadContext.impl->sync.audioWait = false;
		m_threadContext.impl->sync.videoFrameWait = false;
	}
}

void CoreController::showResetInfo(bool enable) {
	m_showResetInfo = enable;
}

void CoreController::setRewinding(bool rewind) {
	if (!m_threadContext.core->opts.rewindEnable) {
		if (rewind) {
			emit statusPosted(tr("Rewinding not currently enabled"));
		}
		return;
	}
	if (rewind && m_multiplayer && m_multiplayer->attached() > 1) {
		return;
	}

	if (rewind && isPaused()) {
		setPaused(false);
		// TODO: restore autopausing
	}
	mCoreThreadSetRewinding(&m_threadContext, rewind);
}

void CoreController::rewind(int states) {
	if (!m_threadContext.core->opts.rewindEnable) {
		emit statusPosted(tr("Rewinding not currently enabled"));
	}
	Interrupter interrupter(this);
	if (!states) {
		states = INT_MAX;
	}
	mCoreRewindRestore(&m_threadContext.impl->rewind, m_threadContext.core, states);
	interrupter.resume();
	emit frameAvailable();
	emit rewound();
}

void CoreController::setFastForward(bool enable) {
	if (m_fastForward == enable) {
		return;
	}
	m_fastForward = enable;
	updateFastForward();
	emit fastForwardChanged(enable);
}

void CoreController::forceFastForward(bool enable) {
	if (m_fastForwardForced == enable) {
		return;
	}
	m_fastForwardForced = enable;
	updateFastForward();
	emit fastForwardChanged(enable || m_fastForward);
}

void CoreController::changePlayer(int id) {
	Interrupter interrupter(this);
	int playerId = 0;
	mCoreConfigGetIntValue(&m_threadContext.core->config, "savePlayerId", &playerId);
	if (id == playerId) {
		return;
	}
	interrupter.resume();

	QMessageBox* resetPrompt = new QMessageBox(QMessageBox::Question, tr("Reset the game?"),
		tr("Most games will require a reset to load the new save. Do you want to reset now?"),
		QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
	connect(resetPrompt, &QMessageBox::buttonClicked, this, [this, resetPrompt, id](QAbstractButton* button) {
		Interrupter interrupter(this);
		switch (resetPrompt->standardButton(button)) {
		default:
			return;
		case QMessageBox::Yes:
			mCoreConfigSetOverrideIntValue(&m_threadContext.core->config, "savePlayerId", id);
			m_resetActions.append([this]() {
				updatePlayerSave();
			});
			interrupter.resume();
			reset();
			break;
		case QMessageBox::No:
			mCoreConfigSetOverrideIntValue(&m_threadContext.core->config, "savePlayerId", id);
			updatePlayerSave();
			break;
		}
	});
	resetPrompt->setAttribute(Qt::WA_DeleteOnClose);
	resetPrompt->show();
}

void CoreController::overrideMute(bool override) {
	m_mute = override;

	Interrupter interrupter(this);
	mCore* core = m_threadContext.core;
	if (m_mute) {
		core->opts.mute = true;
	} else {
		if (m_fastForward || m_fastForwardForced) {
			core->opts.mute = m_fastForwardMute;
		} else {
			mCoreConfigGetBoolValue(&core->config, "mute", &core->opts.mute);
		}
	}
	core->reloadConfigOption(core, NULL, NULL);
}

void CoreController::loadState(int slot) {
	if (slot > 0 && slot != m_stateSlot) {
		m_stateSlot = slot;
		m_backupSaveState.clear();
	}
	m_crashSeen = false;
	mCoreThreadClearCrashed(&m_threadContext);
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		if (!controller->m_backupLoadState.isOpen()) {
			controller->m_backupLoadState = VFileDevice::openMemory();
		}
		mCoreSaveStateNamed(context->core, controller->m_backupLoadState, controller->m_saveStateFlags);
		if (mCoreLoadState(context->core, controller->m_stateSlot, controller->m_loadStateFlags)) {
			emit controller->frameAvailable();
			emit controller->stateLoaded();
		}
	});
}

void CoreController::loadState(const QString& path, int flags) {
	m_statePath = path;
	int savedFlags = m_loadStateFlags;
	if (flags != -1) {
		m_loadStateFlags = flags;
	}
	m_crashSeen = false;
	mCoreThreadClearCrashed(&m_threadContext);
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = VFileDevice::open(controller->m_statePath, O_RDONLY);
		if (!vf) {
			return;
		}
		if (!controller->m_backupLoadState.isOpen()) {
			controller->m_backupLoadState = VFileDevice::openMemory();
		}
		mCoreSaveStateNamed(context->core, controller->m_backupLoadState, controller->m_saveStateFlags);
		if (mCoreLoadStateNamed(context->core, vf, controller->m_loadStateFlags)) {
			emit controller->frameAvailable();
			emit controller->stateLoaded();
		}
		vf->close(vf);
	});
	m_loadStateFlags = savedFlags;
}

void CoreController::loadState(QIODevice* iodev, int flags) {
	m_stateVf = VFileDevice::wrap(iodev, QIODevice::ReadOnly);
	if (!m_stateVf) {
		return;
	}
	int savedFlags = m_loadStateFlags;
	if (flags != -1) {
		m_loadStateFlags = flags;
	}
	m_crashSeen = false;
	mCoreThreadClearCrashed(&m_threadContext);
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = controller->m_stateVf;
		if (!vf) {
			return;
		}
		if (!controller->m_backupLoadState.isOpen()) {
			controller->m_backupLoadState = VFileDevice::openMemory();
		}
		mCoreSaveStateNamed(context->core, controller->m_backupLoadState, controller->m_saveStateFlags);
		if (mCoreLoadStateNamed(context->core, vf, controller->m_loadStateFlags)) {
			emit controller->frameAvailable();
			emit controller->stateLoaded();
		}
		vf->close(vf);
	});
	m_loadStateFlags = savedFlags;
}

void CoreController::saveState(int slot) {
	if (slot > 0) {
		m_stateSlot = slot;
	}
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = mCoreGetState(context->core, controller->m_stateSlot, false);
		if (vf) {
			controller->m_backupSaveState.resize(vf->size(vf));
			vf->read(vf, controller->m_backupSaveState.data(), controller->m_backupSaveState.size());
			vf->close(vf);
		}
		mCoreSaveState(context->core, controller->m_stateSlot, controller->m_saveStateFlags);
	});
}

void CoreController::saveState(const QString& path, int flags) {
	m_statePath = path;
	int savedFlags = m_saveStateFlags;
	if (flags != -1) {
		m_saveStateFlags = flags;
	}
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = VFileDevice::open(controller->m_statePath, O_RDONLY);
		if (vf) {
			controller->m_backupSaveState.resize(vf->size(vf));
			vf->read(vf, controller->m_backupSaveState.data(), controller->m_backupSaveState.size());
			vf->close(vf);
		}
		vf = VFileDevice::open(controller->m_statePath, O_RDWR | O_CREAT | O_TRUNC);
		if (!vf) {
			return;
		}
		mCoreSaveStateNamed(context->core, vf, controller->m_saveStateFlags);
		vf->close(vf);
	});
	m_saveStateFlags = savedFlags;
}

void CoreController::saveState(QIODevice* iodev, int flags) {
	m_stateVf = VFileDevice::wrap(iodev, QIODevice::WriteOnly | QIODevice::Truncate);
	if (!m_stateVf) {
		return;
	}
	int savedFlags = m_saveStateFlags;
	if (flags != -1) {
		m_saveStateFlags = flags;
	}
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = controller->m_stateVf;
		if (!vf) {
			return;
		}
		mCoreSaveStateNamed(context->core, vf, controller->m_saveStateFlags);
		vf->close(vf);
	});
	m_saveStateFlags = savedFlags;
}

void CoreController::loadBackupState() {
	if (!m_backupLoadState.isOpen()) {
		return;
	}

	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		controller->m_backupLoadState.seek(0);
		if (mCoreLoadStateNamed(context->core, controller->m_backupLoadState, controller->m_loadStateFlags)) {
			mLOG(STATUS, INFO, "Undid state load");
			controller->frameAvailable();
			controller->stateLoaded();
		}
		controller->m_backupLoadState.close();
	});
}

void CoreController::saveBackupState() {
	if (m_backupSaveState.isEmpty()) {
		return;
	}

	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		CoreController* controller = static_cast<CoreController*>(context->userData);
		VFile* vf = mCoreGetState(context->core, controller->m_stateSlot, true);
		if (vf) {
			vf->write(vf, controller->m_backupSaveState.constData(), controller->m_backupSaveState.size());
			vf->close(vf);
			mLOG(STATUS, INFO, "Undid state save");
		}
		controller->m_backupSaveState.clear();
	});
}

void CoreController::loadSave(const QString& path, bool temporary) {
	m_resetActions.append([this, path, temporary]() {
		VFile* vf = VFileDevice::open(path, temporary ? O_RDONLY : O_RDWR);
		if (!vf) {
			LOG(QT, ERROR) << tr("Failed to open save file: %1").arg(path);
			return;
		}

		bool ok;
		if (temporary) {
			ok = m_threadContext.core->loadTemporarySave(m_threadContext.core, vf);
		} else {
			ok = m_threadContext.core->loadSave(m_threadContext.core, vf);
		}
		if (!ok) {
			vf->close(vf);
		} else {
			m_savePath = path;
		}
	});
	if (hasStarted()) {
		reset();
	}
}

void CoreController::loadSave(VFile* vf, bool temporary, const QString& path) {
	m_resetActions.append([this, vf, temporary, path]() {
		bool ok;
		if (temporary) {
			ok = m_threadContext.core->loadTemporarySave(m_threadContext.core, vf);
		} else {
			ok = m_threadContext.core->loadSave(m_threadContext.core, vf);
		}
		if (!ok) {
			vf->close(vf);
		} else {
			m_savePath = path;
		}
	});
	if (hasStarted()) {
		reset();
	}
}

void CoreController::loadPatch(const QString& patchPath) {
	Interrupter interrupter(this);
	VFile* patch = VFileDevice::open(patchPath, O_RDONLY);
	if (patch) {
		m_threadContext.core->loadPatch(m_threadContext.core, patch);
		m_patched = true;
		patch->close(patch);
		updateROMInfo();
	}
	if (mCoreThreadHasStarted(&m_threadContext)) {
		interrupter.resume();
		reset();
	}
}

void CoreController::replaceGame(const QString& path) {
	QFileInfo info(path);
	if (!info.isReadable()) {
		LOG(QT, ERROR) << tr("Failed to open game file: %1").arg(path);
		return;
	}
	QString fname = info.canonicalFilePath();
	Interrupter interrupter(this);
	mDirectorySetDetachBase(&m_threadContext.core->dirs);
	if (m_preload) {
		mCorePreloadFile(m_threadContext.core, fname.toUtf8().constData());
	} else {
		mCoreLoadFile(m_threadContext.core, fname.toUtf8().constData());
	}
	// TODO: Make sure updating the path is handled properly by everything that calls path() and baseDirectory()
	updateROMInfo();
}

void CoreController::yankPak() {
	Interrupter interrupter(this);

	switch (platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		GBAYankROM(static_cast<GBA*>(m_threadContext.core->board));
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBYankROM(static_cast<GB*>(m_threadContext.core->board));
		break;
#endif
	case mPLATFORM_NONE:
		LOG(QT, ERROR) << tr("Can't yank pack in unexpected platform!");
		break;
	}
}

void CoreController::addKey(int key) {
	m_activeKeys |= 1 << key;
}

void CoreController::clearKey(int key) {
	m_activeKeys &= ~(1 << key);
	m_removedKeys |= 1 << key;
}

void CoreController::setAutofire(int key, bool enable) {
	if (key >= 32 || key < 0) {
		return;
	}

	m_autofire[key] = enable;
	m_autofireStatus[key] = 0;
}

#ifdef USE_PNG
void CoreController::screenshot() {
	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* context) {
		mCoreTakeScreenshot(context->core);
	});
}
#endif

void CoreController::setRealTime() {
	m_threadContext.core->rtc.override = RTC_NO_OVERRIDE;
}

void CoreController::setFixedTime(const QDateTime& time) {
	m_threadContext.core->rtc.override = RTC_FIXED;
	m_threadContext.core->rtc.value = time.toMSecsSinceEpoch();
}

void CoreController::setFakeEpoch(const QDateTime& time) {
	m_threadContext.core->rtc.override = RTC_FAKE_EPOCH;
	m_threadContext.core->rtc.value = time.toMSecsSinceEpoch();
}

void CoreController::setTimeOffset(qint64 offset) {
	m_threadContext.core->rtc.override = RTC_WALLCLOCK_OFFSET;
	m_threadContext.core->rtc.value = offset * 1000LL;
}

void CoreController::scanCard(const QString& path) {
#ifdef M_CORE_GBA
	QImage image(path);
	if (image.isNull()) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		QByteArray eReaderData = file.read(2912);
		if (eReaderData.isEmpty()) {
			return;
		}

		file.seek(0);
		QStringList lines;
		QDir basedir(QFileInfo(path).dir());

		while (true) {
			QByteArray line = file.readLine().trimmed();
			if (line.isEmpty()) {
				break;
			}
			QString filepath(QString::fromUtf8(line));
			if (filepath.isEmpty() || filepath[0] == QChar('#')) {
				continue;
			}
			if (QFileInfo(filepath).isRelative()) {
				lines.append(basedir.filePath(filepath));
			} else {
				lines.append(filepath);
			}
		}
		scanCards(lines);
		m_eReaderData = std::move(eReaderData);
	} else if (image.size() == QSize(989, 44) || image.size() == QSize(639, 44)) {
		const uchar* bits = image.constBits();
		size_t size;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
		size = image.sizeInBytes();
#else
		size = image.byteCount();
#endif
		m_eReaderData.setRawData(reinterpret_cast<const char*>(bits), size);
	}

	mCoreThreadRunFunction(&m_threadContext, [](mCoreThread* thread) {
		CoreController* controller = static_cast<CoreController*>(thread->userData);
		GBACartEReaderQueueCard(static_cast<GBA*>(thread->core->board), controller->m_eReaderData.constData(), controller->m_eReaderData.size());
	});
#endif
}

void CoreController::scanCards(const QStringList& paths) {
	for (const QString& path : paths) {
		scanCard(path);
	}
}

void CoreController::importSharkport(const QString& path) {
#ifdef M_CORE_GBA
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	VFile* vf = VFileDevice::open(path, O_RDONLY);
	if (!vf) {
		LOG(QT, ERROR) << tr("Failed to open snapshot file for reading: %1").arg(path);
		return;
	}
	Interrupter interrupter(this);
	GBASavedataImportSharkPort(static_cast<GBA*>(m_threadContext.core->board), vf, false);
	GBASavedataImportGSV(static_cast<GBA*>(m_threadContext.core->board), vf, false);
	vf->close(vf);
#endif
}

void CoreController::exportSharkport(const QString& path) {
#ifdef M_CORE_GBA
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	VFile* vf = VFileDevice::open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		LOG(QT, ERROR) << tr("Failed to open snapshot file for writing: %1").arg(path);
		return;
	}
	Interrupter interrupter(this);
	GBASavedataExportSharkPort(static_cast<GBA*>(m_threadContext.core->board), vf);
	vf->close(vf);
#endif
}

#ifdef M_CORE_GB
void CoreController::attachPrinter() {
	if (platform() != mPLATFORM_GB) {
		return;
	}
	GB* gb = static_cast<GB*>(m_threadContext.core->board);
	clearMultiplayerController();
	GBPrinterCreate(&m_printer);
	m_printer.parent = this;
	m_printer.print = [](GBPrinter* printer, int height, const uint8_t* data) {
		QGBPrinter* qPrinter = reinterpret_cast<QGBPrinter*>(printer);
		QImage image(GB_VIDEO_HORIZONTAL_PIXELS, height, QImage::Format_Indexed8);
		QVector<QRgb> colors;
		colors.append(qRgb(0xF8, 0xF8, 0xF8));
		colors.append(qRgb(0xA8, 0xA8, 0xA8));
		colors.append(qRgb(0x50, 0x50, 0x50));
		colors.append(qRgb(0x00, 0x00, 0x00));
		image.setColorTable(std::move(colors));
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < GB_VIDEO_HORIZONTAL_PIXELS; x += 4) {
				uint8_t byte = data[(x + y * GB_VIDEO_HORIZONTAL_PIXELS) / 4];
				image.setPixel(x + 0, y, (byte & 0xC0) >> 6);
				image.setPixel(x + 1, y, (byte & 0x30) >> 4);
				image.setPixel(x + 2, y, (byte & 0x0C) >> 2);
				image.setPixel(x + 3, y, (byte & 0x03) >> 0);
			}
		}
		QMetaObject::invokeMethod(qPrinter->parent, "imagePrinted", Q_ARG(const QImage&, image));
	};
	Interrupter interrupter(this);
	GBSIOSetDriver(&gb->sio, &m_printer.d);
}

void CoreController::detachPrinter() {
	if (platform() != mPLATFORM_GB) {
		return;
	}
	Interrupter interrupter(this);
	GB* gb = static_cast<GB*>(m_threadContext.core->board);
	GBPrinterDonePrinting(&m_printer);
	GBSIOSetDriver(&gb->sio, nullptr);
}

void CoreController::endPrint() {
	if (platform() != mPLATFORM_GB) {
		return;
	}
	Interrupter interrupter(this);
	GBPrinterDonePrinting(&m_printer);
}
#endif

#ifdef M_CORE_GBA
void CoreController::attachBattleChipGate() {
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	Interrupter interrupter(this);
	clearMultiplayerController();
	GBASIOBattlechipGateCreate(&m_battlechip);
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, &m_battlechip);
}

void CoreController::detachBattleChipGate() {
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	Interrupter interrupter(this);
	m_threadContext.core->setPeripheral(m_threadContext.core, mPERIPH_GBA_LINK_PORT, nullptr);
}

void CoreController::setBattleChipId(uint16_t id) {
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	Interrupter interrupter(this);
	m_battlechip.chipId = id;
}

void CoreController::setBattleChipFlavor(int flavor) {
	if (platform() != mPLATFORM_GBA) {
		return;
	}
	Interrupter interrupter(this);
	m_battlechip.flavor = flavor;
}
#endif

void CoreController::setAVStream(mAVStream* stream) {
	Interrupter interrupter(this);
	m_threadContext.core->setAVStream(m_threadContext.core, stream);
}

void CoreController::clearAVStream() {
	Interrupter interrupter(this);
	m_threadContext.core->setAVStream(m_threadContext.core, nullptr);
}

void CoreController::clearOverride() {
	m_override.reset();
}

void CoreController::startVideoLog(const QString& path, bool compression) {
	if (m_vl) {
		return;
	}

	VFile* vf = VFileDevice::open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		return;
	}
	startVideoLog(vf, compression);
}

void CoreController::startVideoLog(VFile* vf, bool compression) {
	if (m_vl || !vf) {
		return;
	}

	Interrupter interrupter(this);
	m_vl = mVideoLogContextCreate(m_threadContext.core);
	m_vlVf = vf;
	mVideoLogContextSetOutput(m_vl, m_vlVf);
	mVideoLogContextSetCompression(m_vl, compression);
	mVideoLogContextWriteHeader(m_vl, m_threadContext.core);
}

void CoreController::endVideoLog(bool closeVf) {
	if (!m_vl) {
		return;
	}

	Interrupter interrupter(this);
	mVideoLogContextDestroy(m_threadContext.core, m_vl, closeVf);
	if (closeVf) {
		m_vlVf = nullptr;
	}
	m_vl = nullptr;
}

void CoreController::setFramebufferHandle(int fb) {
	Interrupter interrupter(this);
	if (fb < 0) {
		if (!m_hwaccel) {
			return;
		}
		mCoreConfigSetIntValue(&m_threadContext.core->config, "hwaccelVideo", 0);
		m_threadContext.core->setVideoGLTex(m_threadContext.core, -1);
		m_hwaccel = false;
	} else {
		mCoreConfigSetIntValue(&m_threadContext.core->config, "hwaccelVideo", 1);
		m_threadContext.core->setVideoGLTex(m_threadContext.core, fb);
		if (m_hwaccel) {
			return;
		}
		m_hwaccel = true;
	}
	if (hasStarted()) {
		m_threadContext.core->reloadConfigOption(m_threadContext.core, "hwaccelVideo", NULL);
		if (!m_hwaccel) {
			m_threadContext.core->setVideoBuffer(m_threadContext.core, reinterpret_cast<mColor*>(m_activeBuffer.data()), screenDimensions().width());
		}
	}
}

void CoreController::updateKeys() {
	int polledKeys = m_inputController->pollEvents() | updateAutofire();
	int activeKeys = m_activeKeys | polledKeys;
	activeKeys |= m_threadContext.core->getKeys(m_threadContext.core) & ~m_removedKeys;
	m_removedKeys = polledKeys;
	if (m_lan2Running) {
		/* v2 netplay: the runner applies exactly the masks it broadcasts to
		 * the peer, so the frontend must not mutate the core mid-frame. */
		m_lan2LocalKeys = activeKeys;
		return;
	}
	m_threadContext.core->setKeys(m_threadContext.core, activeKeys);
}

int CoreController::updateAutofire() {
	int active = 0;
	for (int k = 0; k < 32; ++k) {
		if (!m_autofire[k]) {
			continue;
		}
		++m_autofireStatus[k];
		if (m_autofireStatus[k] >= 2 * m_autofireThreshold) {
			m_autofireStatus[k] = 0;
		} else if (m_autofireStatus[k] >= m_autofireThreshold) {
			active |= 1 << k;
		}
	}
	return active;
}

void CoreController::finishFrame() {
	if (!m_hwaccel) {
		unsigned width, height;
		m_threadContext.core->currentVideoSize(m_threadContext.core, &width, &height);

		QMutexLocker locker(&m_bufferMutex);
		memcpy(m_completeBuffer.data(), m_activeBuffer.constData(), width * height * BYTES_PER_PIXEL);
	}

	{
		QMutexLocker locker(&m_actionMutex);
		QList<std::function<void ()>> frameActions(m_frameActions);
		m_frameActions.clear();
		for (auto& action : frameActions) {
			action();
		}
		if (m_moreFrames > 0) {
			--m_moreFrames;
			if (!m_moreFrames) {
				mCoreThreadPauseFromThread(&m_threadContext);
			}
		}
		++m_frameCounter;
	}
	updateKeys();

	QMetaObject::invokeMethod(this, "frameAvailable");
}

void CoreController::updatePlayerSave() {
	if (m_saveBlocked) {
		return;
	}
	int savePlayerId = m_multiplayer->saveId(this);

	QString saveSuffix;
	if (savePlayerId < 2) {
		saveSuffix = QLatin1String(".sav");
	} else {
		saveSuffix = QString(".sa%1").arg(savePlayerId);
	}
	QByteArray saveSuffixBin(saveSuffix.toUtf8());
	VFile* save = mDirectorySetOpenSuffix(&m_threadContext.core->dirs, m_threadContext.core->dirs.save, saveSuffixBin.constData(), O_CREAT | O_RDWR);
	if (save) {
		if (!m_threadContext.core->loadSave(m_threadContext.core, save)) {
			save->close(save);
		} else {
			if (m_threadContext.core->dirs.save == m_threadContext.core->dirs.base) {
				m_savePath = m_baseDirectory;
			} else {
				m_savePath = QString::fromUtf8(m_threadContext.core->opts.savegamePath);
			}
			m_savePath += PATH_SEP + QString::fromUtf8(m_threadContext.core->dirs.baseName) + saveSuffix;
		}
	}
}

void CoreController::updateFastForward() {
	// If we have "Fast forward" checked in the menu (m_fastForwardForced)
	// or are holding the fast forward button (m_fastForward):
	if (m_fastForward || m_fastForwardForced) {
		if (m_fastForwardVolume >= 0) {
			m_threadContext.core->opts.volume = m_fastForwardVolume;
		}
		m_threadContext.core->opts.mute = m_fastForwardMute || m_mute;
		setSync(false);

		// If we aren't holding the fast forward button
		// then use the non "(held)" ratio
		if(!m_fastForward) {
			if (m_fastForwardRatio > 0) {
				m_threadContext.impl->sync.fpsTarget = m_fpsTarget * m_fastForwardRatio;
				m_threadContext.impl->sync.audioWait = true;
			}
		} else {
			// If we are holding the fast forward button,
			// then use the held ratio
			if (m_fastForwardHeldRatio > 0) {
				m_threadContext.impl->sync.fpsTarget = m_fpsTarget * m_fastForwardHeldRatio;
				m_threadContext.impl->sync.audioWait = true;
			}
		}
	} else {
		if (!mCoreConfigGetIntValue(&m_threadContext.core->config, "volume", &m_threadContext.core->opts.volume)) {
			m_threadContext.core->opts.volume = 0x100;
		}
		mCoreConfigGetBoolValue(&m_threadContext.core->config, "mute", &m_threadContext.core->opts.mute);
		m_threadContext.impl->sync.fpsTarget = m_fpsTarget;
		setSync(true);
	}

	m_threadContext.core->reloadConfigOption(m_threadContext.core, NULL, NULL);
}

void CoreController::updateROMInfo() {
	const NoIntroDB* db = GBAApp::app()->gameDB();
	NoIntroGame game{};
	m_crc32 = 0;
	mCore* core = m_threadContext.core;
	core->checksum(core, &m_crc32, mCHECKSUM_CRC32);

	mGameInfo info;
	core->getGameInfo(core, &info);
	m_internalTitle = QLatin1String(info.title);

#ifdef USE_SQLITE3
	if (db && m_crc32 && NoIntroDBLookupGameByCRC(db, m_crc32, &game)) {
		m_dbTitle = QString::fromUtf8(game.name);
	}
#endif
}

CoreController::Interrupter::Interrupter()
	: m_parent(nullptr)
{
}

CoreController::Interrupter::Interrupter(CoreController* parent)
	: m_parent(parent)
{
	interrupt();
}

CoreController::Interrupter::Interrupter(std::shared_ptr<CoreController> parent)
	: m_parent(parent.get())
{
	interrupt();
}

CoreController::Interrupter::Interrupter(const Interrupter& other)
	: m_parent(other.m_parent)
{
	interrupt();
}

CoreController::Interrupter::~Interrupter() {
	resume();
}

CoreController::Interrupter& CoreController::Interrupter::operator=(const Interrupter& other)
{
	interrupt(other.m_parent);
	return *this;
}

void CoreController::Interrupter::interrupt(CoreController* controller) {
	if (m_parent != controller) {
		CoreController* old = m_parent;
		m_parent = controller;
		interrupt();
		resume(old);
	}
}

void CoreController::Interrupter::interrupt(std::shared_ptr<CoreController> controller) {
	interrupt(controller.get());
}

void CoreController::Interrupter::interrupt() {
	if (!m_parent || !m_parent->thread()->impl) {
		return;
	}

	if (mCoreThreadGet() != m_parent->thread()) {
		mCoreThreadInterrupt(m_parent->thread());
	} else {
		mCoreThreadInterruptFromThread(m_parent->thread());
	}
}

void CoreController::Interrupter::resume() {
	resume(m_parent);
	m_parent = nullptr;
}

void CoreController::Interrupter::resume(CoreController* controller) {
	if (!controller || !controller->thread()->impl) {
		return;
	}

	mCoreThreadContinue(controller->thread());
}

bool CoreController::Interrupter::held() const {
	return m_parent && m_parent->thread()->impl;
}
