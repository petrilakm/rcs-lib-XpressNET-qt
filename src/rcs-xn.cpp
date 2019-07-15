#include <algorithm>
#include <cstring>

#include "errors.h"
#include "rcs-xn.h"
#include "util.h"

namespace RcsXn {

AppThread main_thread;
RcsXn rx;

///////////////////////////////////////////////////////////////////////////////

RcsXn::RcsXn(QObject *parent) : QObject(parent), xn(this) {
	QObject::connect(&xn, SIGNAL(onError(QString)), this, SLOT(xnOnError(QString)));
	QObject::connect(&xn, SIGNAL(onLog(QString, Xn::XnLogLevel)), this, SLOT(xnOnLog(QString, Xn::XnLogLevel)));
	QObject::connect(&xn, SIGNAL(onConnect()), this, SLOT(xnOnConnect()));
	QObject::connect(&xn, SIGNAL(onDisconnect()), this, SLOT(xnOnDisconnect()));
	QObject::connect(&xn, SIGNAL(onTrkStatusChanged(Xn::XnTrkStatus)), this, SLOT(xnOnTrkStatusChanged(Xn::XnTrkStatus)));
	QObject::connect(&xn, SIGNAL(onAccInputChanged(uint8_t, bool, bool, Xn::XnFeedbackType, Xn::XnAccInputsState)),
					 this, SLOT(xnOnAccInputChanged(uint8_t, bool, bool, Xn::XnFeedbackType, Xn::XnAccInputsState)));

	// No loading of configuration here (caller should call LoadConfig)
}

RcsXn::~RcsXn() {
	try {
		if (xn.connected())
			close();
		if (this->config_filename != "")
			s.save(this->config_filename);
	} catch (...) {
		// No exceptions in destructor!
	}
}

void RcsXn::log(const QString &msg, RcsXnLogLevel loglevel) {
	if (loglevel <= this->loglevel)
		this->events.call(this->events.onLog, static_cast<int>(loglevel), msg);
}

void RcsXn::error(const QString &message, uint16_t code, unsigned int module) {
	this->events.call(this->events.onError, code, module, message);
}

void RcsXn::error(const QString &message, uint16_t code) { this->error(message, code, 0); }
void RcsXn::error(const QString &message) { this->error(message, RCS_GENERAL_EXCEPTION, 0); }

int RcsXn::openDevice(const QString &device, bool persist) {
	events.call(rx.events.beforeOpen);

	if (xn.connected())
		return RCS_ALREADY_OPENNED;

	log("Connecting to XN...", RcsXnLogLevel::llInfo);

	try {
		xn.connect(device, s["XN"]["baudrate"].toInt(),
		           static_cast<QSerialPort::FlowControl>(s["XN"]["flowcontrol"].toInt()));
	} catch (const Xn::QStrException &e) {
		error("XN connect error while opening serial port '" +
		      s["XN"]["port"].toString() + "':" + e, RCS_CANNOT_OPEN_PORT);
		return RCS_CANNOT_OPEN_PORT;
	}

	if (persist)
		s["XN"]["port"] = device;

	return 0;
}

int RcsXn::close() {
	events.call(rx.events.beforeClose);

	if (!xn.connected())
		return RCS_NOT_OPENED;

	if (this->started > RcsStartState::stopped)
		return RCS_SCANNING_NOT_FINISHED;

	log("Disconnecting from XN...", RcsXnLogLevel::llInfo);
	this->opening = false;
	try {
		xn.disconnect();
	} catch (const Xn::QStrException &e) {
		error("XN disconnect error while closing serial port:" + e);
	}

	return 0;
}

int RcsXn::start() {
	if (this->started == RcsStartState::started)
		return RCS_ALREADY_STARTED;
	if (this->started == RcsStartState::scanning)
		return RCS_SCANNING_NOT_FINISHED;
	if (!xn.connected())
		return RCS_NOT_OPENED;

	events.call(rx.events.beforeStart);
	started = RcsStartState::scanning;
	events.call(rx.events.afterStart);
	this->first_scan();
	return 0;
}

int RcsXn::stop() {
	if (rx.started == RcsStartState::stopped)
		return RCS_NOT_STARTED;

	events.call(rx.events.beforeStop);
	this->started = RcsStartState::stopped;
	events.call(rx.events.afterStop);
	return 0;
}

void RcsXn::loadConfig(const QString &filename) {
	s.load(filename);
	this->loglevel = static_cast<RcsXnLogLevel>(s["XN"]["loglevel"].toInt());
	this->xn.loglevel = static_cast<Xn::XnLogLevel>(s["XN"]["loglevel"].toInt());
	this->parseActiveModules(s["modules"]["active"].toString());
}

void RcsXn::first_scan() {
	this->scan_group = 0;
	this->scan_nibble = false;
	xn.accInfoRequest(
		0, false,
		std::make_unique<Xn::XnCb>([this](void *s, void *d) { xnOnInitScanningError(s, d); })
	);
}

void RcsXn::initModuleScanned(uint8_t group, bool nibble) {
	if (group == ((IO_MODULES_COUNT/4)-1) && nibble) {
		this->initScanningDone();
		return;
	}

	if (!nibble) {
		nibble = true;
	} else {
		group++;
		nibble = false;
	}

	this->scan_group = group;
	this->scan_nibble = nibble;

	xn.accInfoRequest(
		group, nibble,
		std::make_unique<Xn::XnCb>([this](void *s, void *d) { xnOnInitScanningError(s, d); })
	);
}

void RcsXn::xnOnInitScanningError(void *, void *) {
	error("Module scanning: no response!", RCS_NOT_OPENED);
	this->stop();
}

void RcsXn::initScanningDone() {
	this->started = RcsStartState::started;
	events.call(events.onScanned);
}

void RcsXn::xnSetOutputError(void *sender, void *data) {
	(void)sender;
	// TODO: mark module as failed?
	unsigned int module = reinterpret_cast<intptr_t>(data);
	error("Command Station did not respond to SetOutput command!", RCS_MODULE_NOT_ANSWERED_CMD,
	      module);
}

void RcsXn::parseActiveModules(const QString &active) {
	std::fill(this->active.begin(), this->active.end(), false);
	const QStringList ranges = active.split(',');
	for (const QString& range : ranges) {
		const QStringList bounds = range.split('-');
		bool okl, okr = false;
		if (bounds.size() == 2) {
			unsigned int addr = bounds[0].toUInt(&okl);
			if (okl)
				this->active[addr] = true;
			else
				log("Invalid range: " + bounds[0], RcsXnLogLevel::llWarning);
		} else if (bounds.size() == 1) {
			unsigned int left = bounds[0].toUInt(&okl);
			unsigned int right = bounds[1].toUInt(&okr);
			if (okl & okr) {
				for (size_t i = left; i <= right; i++)
					this->active[i] = true;
			} else
				log("Invalid range: " + range, RcsXnLogLevel::llWarning);
		} else {
			log("Invalid range: " + range, RcsXnLogLevel::llWarning);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Xn events

void RcsXn::xnOnError(QString error) { this->error(error); }

void RcsXn::xnOnLog(QString message, Xn::XnLogLevel loglevel) {
	this->log(message, static_cast<RcsXnLogLevel>(loglevel));
}

void RcsXn::xnOnConnect() {
	this->opening = true;

	try {
		xn.getLIVersion(
		    [this](void *s, unsigned hw, unsigned sw) { xnGotLIVersion(s, hw, sw); },
		    std::make_unique<Xn::XnCb>([this](void *s, void *d) { xnOnLIVersionError(s, d); })
		);
	}
	catch (const Xn::QStrException& e) {
		error("Get LI Version: " + e.str(), RCS_NOT_OPENED);
		this->close();
	}
}

void RcsXn::xnOnDisconnect() { this->events.call(this->events.afterClose); }

void RcsXn::xnOnTrkStatusChanged(Xn::XnTrkStatus s) {
	(void)s;
	// Nothing here yet.
}

void RcsXn::xnOnAccInputChanged(uint8_t groupAddr, bool nibble, bool error,
                                Xn::XnFeedbackType inputType, Xn::XnAccInputsState state) {
	(void)error; // ignoring errors reported by decoders
	(void)inputType; // ignoring input type reported by decoder

	unsigned int port = 8*groupAddr + 4*nibble;
	this->inputs[port+0] = state.sep.i0;
	this->inputs[port+1] = state.sep.i1;
	this->inputs[port+2] = state.sep.i2;
	this->inputs[port+3] = state.sep.i3;

	if ((this->started == RcsStartState::scanning) && (groupAddr == this->scan_group) &&
	    (nibble == this->scan_nibble)) {
		this->initModuleScanned(groupAddr, nibble);
	} else {
		events.call(events.onInputChanged, port/2);
	}
}

void RcsXn::xnOnLIVersionError(void *, void *) {
	error("Get LI Version: no response!", RCS_NOT_OPENED);
	this->close();
}

void RcsXn::xnOnCSStatusError(void *, void *) {
	error("Get CS Status: no response!", RCS_NOT_OPENED);
	this->close();
}

void RcsXn::xnOnCSStatusOk(void *, void *) {
	// Device opened
	this->opening = false;
	this->events.call(this->events.afterOpen);
}

void RcsXn::xnGotLIVersion(void*, unsigned hw, unsigned sw) {
	log("Got LI version. HW: " + QString::number(hw) + ", SW: " + QString::number(sw),
	    RcsXnLogLevel::llInfo);
	this->li_ver_hw = hw;
	this->li_ver_sw = sw;

	try {
		xn.getCommandStationStatus(
		    std::make_unique<Xn::XnCb>([this](void *s, void *d) { xnOnCSStatusOk(s, d); }),
		    std::make_unique<Xn::XnCb>([this](void *s, void *d) { xnOnCSStatusError(s, d); })
		);
	}
	catch (const Xn::QStrException& e) {
		error("Get CS Status: " + e.str(), RCS_NOT_OPENED);
		this->close();
	}
}

///////////////////////////////////////////////////////////////////////////////
// Open/close

int Open() { return rx.openDevice(rx.s["XN"]["port"].toString(), false); }

int OpenDevice(char16_t *device, bool persist) {
	return rx.openDevice(QString::fromUtf16(device), persist);
}

int Close() { return rx.close(); }
bool Opened() { return (rx.xn.connected() && (!rx.opening)); }

///////////////////////////////////////////////////////////////////////////////
// Start/stop

int Start() { return rx.start(); }
int Stop() { return rx.stop(); }
bool Started() { return (rx.started > RcsStartState::stopped); }

///////////////////////////////////////////////////////////////////////////////
// Config

int LoadConfig(char16_t *filename) {
	if (rx.xn.connected())
		return RCS_FILE_DEVICE_OPENED;
	try {
		rx.config_filename = QString::fromUtf16(filename);
		rx.loadConfig(QString::fromUtf16(filename));
	} catch (...) {
		return RCS_FILE_CANNOT_ACCESS;
	}
	return 0;
}

int SaveConfig(char16_t *filename) {
	try {
		rx.s.save(QString::fromUtf16(filename));
	} catch (...) {
		return RCS_FILE_CANNOT_ACCESS;
	}
	return 0;
}

void SetConfigFileName(char16_t *filename) {
	rx.config_filename = QString::fromUtf16(filename);
}

///////////////////////////////////////////////////////////////////////////////
// Loglevel

void SetLogLevel(unsigned int loglevel) {
	rx.loglevel = static_cast<RcsXnLogLevel>(loglevel);
	rx.xn.loglevel = static_cast<Xn::XnLogLevel>(loglevel);
	rx.s["XN"]["loglevel"] = loglevel;
}

unsigned int GetLogLevel() { return static_cast<unsigned int>(rx.loglevel); }

///////////////////////////////////////////////////////////////////////////////
// RCS IO

int GetInput(unsigned int module, unsigned int port) {
	if (rx.started == RcsStartState::stopped)
		return RCS_NOT_STARTED;
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	if (port >= IO_MODULE_PIN_COUNT) {
		#ifdef IGNORE_PIN_BOUNDS
		return 0;
		#else
		return RCS_PORT_INVALID_NUMBER;
		#endif
	}
	if (rx.started == RcsStartState::scanning)
		return RCS_INPUT_NOT_YET_SCANNED;

	return rx.inputs[module*2 + port];
}

int GetOutput(unsigned int module, unsigned int port) {
	if (rx.started == RcsStartState::stopped)
		return RCS_NOT_STARTED;
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	if (port >= IO_MODULE_PIN_COUNT) {
		#ifdef IGNORE_PIN_BOUNDS
		return 0;
		#else
		return RCS_PORT_INVALID_NUMBER;
		#endif
	}

	return rx.outputs[module*2 + port];
}

int SetOutput(unsigned int module, unsigned int port, int state) {
	if (rx.started == RcsStartState::stopped)
		return RCS_NOT_STARTED;
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	if (port >= IO_MODULE_PIN_COUNT) {
		#ifdef IGNORE_PIN_BOUNDS
		return 0;
		#else
		return RCS_PORT_INVALID_NUMBER;
		#endif
	}

	unsigned int portAddr = (module<<1) + (port&1); // 0-2048
	rx.outputs[portAddr] = state;
	rx.xn.accOpRequest(
	    portAddr, state, nullptr,
		std::make_unique<Xn::XnCb>([](void *s, void *d) { rx.xnSetOutputError(s, d); },
	                               reinterpret_cast<void *>(module))
	);
	rx.events.call(rx.events.onOutputChanged, module); // TODO: move to ok callback?
	return 0;
}

int GetInputType(unsigned int module, unsigned int port) {
	(void)module;
	(void)port;
	return 0; // all inputs are plain inputs
}

int GetOutputType(unsigned int module, unsigned int port) {
	(void)module;
	(void)port;
	return 0; // al output are plain outputs yet
}

///////////////////////////////////////////////////////////////////////////////
// Devices

int GetDeviceCount() {
	return 1;
}

void GetDeviceSerial(int index, char16_t *serial, unsigned int serialLen) {
	(void)index;
	const QString sname = "COM port";
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(sname.utf16()), serial, serialLen);
}

///////////////////////////////////////////////////////////////////////////////
// Module questionaries

unsigned int GetModuleCount() { return IO_MODULES_COUNT; }

bool IsModule(unsigned int module) {
	return (module < IO_MODULES_COUNT); // XpressNET provides no info about module existence
}

bool IsModuleFailure(unsigned int module) {
	(void)module;
	return false; // XpressNET provides no info about module failure
}

int GetModuleTypeStr(unsigned int module, char16_t *type, unsigned int typeLen) {
	(void)module;
	const QString stype = "XN";
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(stype.utf16()), type, typeLen);
	return 0;
}

int GetModuleName(unsigned int module, char16_t *name, unsigned int nameLen) {
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	const QString str = "Module " + QString::number(module);
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(str.utf16()), name, nameLen);
	return 0;
}

int GetModuleFW(unsigned int module, char16_t *fw, unsigned int fwLen) {
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	const QString sfw = "-";
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(sfw.utf16()), fw, fwLen);
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Versions

bool ApiSupportsVersion(unsigned int version) {
	return std::find(API_SUPPORTED_VERSIONS.begin(), API_SUPPORTED_VERSIONS.end(), version) !=
	       API_SUPPORTED_VERSIONS.end();
}

int ApiSetVersion(unsigned int version) {
	if (!ApiSupportsVersion(version))
		return RCS_UNSUPPORTED_API_VERSION;

	rx.api_version = version;
	return 0;
}

unsigned int GetDeviceVersion(char16_t *version, unsigned int versionLen) {
	const QString sversion = "LI HW: " + QString::number(rx.li_ver_hw) + ", LI SW: " +
							 QString::number(rx.li_ver_sw);
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(sversion.utf16()), version,
	                          versionLen);
	return 0;
}

unsigned int GetDriverVersion(char16_t *version, unsigned int versionLen) {
	const QString sversion = VERSION;
	StrUtil::strcpy<char16_t>(reinterpret_cast<const char16_t *>(sversion.utf16()), version,
	                          versionLen);
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// General library configuration

unsigned int GetModuleInputsCount(unsigned int module) {
	(void)module;
	return IO_MODULE_PIN_COUNT;
}

unsigned int GetModuleOutputsCount(unsigned int module) {
	(void)module;
	return IO_MODULE_PIN_COUNT;
}

///////////////////////////////////////////////////////////////////////////////
// Events binders

void BindBeforeOpen(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.beforeOpen, f, data); }
void BindAfterOpen(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.afterOpen, f, data); }
void BindBeforeClose(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.beforeClose, f, data); }
void BindAfterClose(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.afterClose, f, data); }
void BindBeforeStart(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.beforeStart, f, data); }

void BindAfterStart(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.afterStart, f, data); }
void BindBeforeStop(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.beforeStop, f, data); }
void BindAfterStop(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.afterStop, f, data); }
void BindOnError(StdErrorEvent f, void *data) { rx.events.bind(rx.events.onError, f, data); }
void BindOnLog(StdLogEvent f, void *data) { rx.events.bind(rx.events.onLog, f, data); }

void BindOnInputChanged(StdModuleChangeEvent f, void *data) { rx.events.bind(rx.events.onInputChanged, f, data); }
void BindOnOutputChanged(StdModuleChangeEvent f, void *data) { rx.events.bind(rx.events.onOutputChanged, f, data); }
void BindOnScanned(StdNotifyEvent f, void *data) { rx.events.bind(rx.events.onScanned, f, data); }

///////////////////////////////////////////////////////////////////////////////

} // namespace RcsXn
