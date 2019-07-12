#include <cstring>

#include "rcs-xn.h"
#include "errors.h"
#include "util.h"

namespace RcsXn {

RcsXn rx;

///////////////////////////////////////////////////////////////////////////////

RcsXn::RcsXn(QObject *parent)
	: QObject(parent), xn(this) {
	QObject::connect(&xn, SIGNAL(onError()), this, SLOT(xnOnError()));
	QObject::connect(&xn, SIGNAL(onLog()), this, SLOT(xnOnLog()));
	QObject::connect(&xn, SIGNAL(onConnect()), this, SLOT(xnOnConnect()));
	QObject::connect(&xn, SIGNAL(onDisconnect()), this, SLOT(xnOnDisconnect()));
	QObject::connect(&xn, SIGNAL(onTrkStatusChanged()), this, SLOT(xnOnTrkStatusChanged()));
	QObject::connect(&xn, SIGNAL(onAccInputChanged()), this, SLOT(xnOnAccInputChanged()));

	this->loadConfig(CONFIG_FN);
}

RcsXn::~RcsXn() {
	try {
		if (xn.connected())
			close();
		s.save(CONFIG_FN); // optional
	} catch (...) {
		// No exceptions in destructor!
	}
}

void RcsXn::log(const QString& msg, RcsXnLogLevel loglevel) {
	if (loglevel <= this->loglevel)
		this->events.call(this->events.onLog, static_cast<int>(loglevel), msg);
}

void RcsXn::error(const QString& message, uint16_t code, unsigned int module) {
	this->events.call(this->events.onError, code, module, message);
}

void RcsXn::error(const QString& message, uint16_t code) {
	this->error(message, code, 0);
}

void RcsXn::error(const QString& message) {
	this->error(message, RCS_GENERAL_EXCEPTION, 0);
}

int RcsXn::openDevice(const QString& device, bool persist) {
	events.call(rx.events.beforeOpen);

	if (xn.connected())
		return RCS_ALREADY_OPENNED;

	log("Connecting to XN...", RcsXnLogLevel::llInfo);

	try {
		xn.connect(device, s["XN"]["baudrate"].toInt(),
		           static_cast<QSerialPort::FlowControl>(s["XN"]["flowcontrol"].toInt()));
	} catch (const Xn::QStrException& e) {
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
	} catch (const Xn::QStrException& e) {
		error("XN disconnect error while closing serial port:" + e);
	}

	return 0;
}

void RcsXn::loadConfig(const QString& filename) {
	s.load(filename);
	this->loglevel = static_cast<RcsXnLogLevel>(s["XN"]["loglevel"].toInt());
	this->xn.loglevel = static_cast<Xn::XnLogLevel>(s["XN"]["loglevel"].toInt());
}

void RcsXn::first_scan() {
}

void RcsXn::xnSetOutputError(void* sender, void* data) {
	(void)sender;
	// TODO: mark module as failed?
	unsigned int module = reinterpret_cast<intptr_t>(data);
	error("Command Station did not respond to SetOutput command!", RCS_MODULE_NOT_ANSWERED_CMD,
	      module);
}

///////////////////////////////////////////////////////////////////////////////
// Xn events

void RcsXn::xnOnError(QString error) {
	this->error(error);
}

void RcsXn::xnOnLog(QString message, Xn::XnLogLevel loglevel) {
	// TODO: a little loglevel mismatch
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

void RcsXn::xnOnDisconnect() {
	this->events.call(this->events.afterClose);
}

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

	events.call(events.onInputChanged, port/2);
}

void RcsXn::xnOnLIVersionError(void*, void*) {
	error("Get LI Version: no response!", RCS_NOT_OPENED);
	this->close();
}

void RcsXn::xnOnCSStatusError(void*, void*) {
	error("Get CS Status: no response!", RCS_NOT_OPENED);
	this->close();
}

void RcsXn::xnOnCSStatusOk(void*, void*) {
	// Device opened
	this->opening = false;
	this->events.call(this->events.afterOpen);
}

void RcsXn::xnGotLIVersion(void*, unsigned hw, unsigned sw) {
	log("Got LI version. HW: " + QString::number(hw) + ", SW: " + QString::number(sw),
	    RcsXnLogLevel::llInfo);
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

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV Open() {
	return rx.openDevice(rx.s["XN"]["port"].toString(), false);
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV OpenDevice(char16_t* device, bool persist) {
	return rx.openDevice(QString::fromUtf16(device), persist);
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV Close() {
	return rx.close();
}

extern "C" RCS_XN_SHARED_EXPORT bool CALL_CONV Opened() {
	return (rx.xn.connected() && (!rx.opening));
}

///////////////////////////////////////////////////////////////////////////////
// Start/stop

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV Start() {
	if (rx.started > RcsStartState::stopped)
		return RCS_ALREADY_STARTED;
	if (!rx.xn.connected())
		return RCS_NOT_OPENED;
	// TODO : wtf is RCS_SCANNING_NOT_FINISHED?

	rx.events.call(rx.events.beforeStart);
	rx.started = RcsStartState::scanning;
	rx.events.call(rx.events.afterStart);
	rx.first_scan();
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV Stop() {
	if (rx.started == RcsStartState::stopped)
		return RCS_NOT_STARTED;

	rx.events.call(rx.events.beforeStop);
	rx.started = RcsStartState::stopped;
	rx.events.call(rx.events.afterStop);
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT bool CALL_CONV Started() {
	return (rx.started > RcsStartState::stopped);
}

///////////////////////////////////////////////////////////////////////////////
// Config

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV LoadConfig(char16_t* filename) {
	if (rx.xn.connected())
		return RCS_FILE_DEVICE_OPENED;
	try {
		rx.loadConfig(QString::fromUtf16(filename));
	} catch (...) {
		return RCS_FILE_CANNOT_ACCESS;
	}
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV SaveConfig(char16_t* filename) {
	try {
		rx.s.save(QString::fromUtf16(filename));
	} catch (...) {
		return RCS_FILE_CANNOT_ACCESS;
	}
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Loglevel

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV SetLogLevel(unsigned int loglevel) {
	rx.loglevel = static_cast<RcsXnLogLevel>(loglevel);
	rx.xn.loglevel = static_cast<Xn::XnLogLevel>(loglevel);
	rx.s["XN"]["loglevel"] = loglevel;
}

extern "C" RCS_XN_SHARED_EXPORT unsigned int CALL_CONV GetLogLevel() {
	return static_cast<unsigned int>(rx.loglevel);
}

///////////////////////////////////////////////////////////////////////////////
// RCS IO

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetInput(unsigned int module, unsigned int port) {
	if (rx.started != RcsStartState::started)
		return RCS_NOT_STARTED;
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	if (port >= IO_MODULE_PIN_COUNT)
		return RCS_PORT_INVALID_NUMBER;
	
	return rx.inputs[module*2 + port];
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetOutput(unsigned int module, unsigned int port) {
	if (rx.started != RcsStartState::started)
		return RCS_NOT_STARTED;
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	if (port >= IO_MODULE_PIN_COUNT)
		return RCS_PORT_INVALID_NUMBER;

	return rx.outputs[module*2 + port];
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV SetOutput(unsigned int module, unsigned int port, int state) {
	unsigned int portAddr = (module<<1) + (port&1); // 0-2048
	rx.outputs[portAddr] = state;
	rx.xn.accOpRequest(
		portAddr,
		state,
		nullptr,
		std::make_unique<Xn::XnCb>([](void *s, void *d) { rx.xnSetOutputError(s, d); }, reinterpret_cast<void*>(module))
	);
	rx.events.call(rx.events.onOutputChanged, module); // TODO: move to ok callback?
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetInputType(unsigned int module, unsigned int port) {
	(void)module;
	(void)port;
	return 0; // all inputs are plain inputs
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetOutputType(unsigned int module, unsigned int port) {
	(void)module;
	(void)port;
	return 0; // al output are plain outputs yet
}

///////////////////////////////////////////////////////////////////////////////
// Config dialogs

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV ShowConfigDialog() { /* Nothing here intentionally */ }
extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV HideConfigDialog() { /* Nothing here intentionally */ }

///////////////////////////////////////////////////////////////////////////////
// Module qustionaries

extern "C" RCS_XN_SHARED_EXPORT bool CALL_CONV IsModule(unsigned int module) {
	(void)module;
	return true; // XpressNET provides no info about module existence
}

extern "C" RCS_XN_SHARED_EXPORT bool CALL_CONV IsModuleFailure(unsigned int module) {
	(void)module;
	return false; // XpressNET provides no info about module failure
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetModuleTypeStr(char16_t* type, unsigned int typeLen) {
	// TODO WTF is signature of this function?
	const char16_t* type_utf16 = reinterpret_cast<const char16_t*>(QString("XN").utf16());
	StrUtil::strcpy<char16_t>(type_utf16, type, typeLen);
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetModuleName(unsigned int module, char16_t* name,
                                                 unsigned int nameLen) {
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	const char16_t* name_utf16 = reinterpret_cast<const char16_t*>(
		QString("Module "+QString::number(module)).utf16()
	);
	StrUtil::strcpy<char16_t>(name_utf16, name, nameLen);
	return 0;
}

extern "C" RCS_XN_SHARED_EXPORT int CALL_CONV GetModuleFW(unsigned int module, char16_t* fw, unsigned int fwLen) {
	if (module >= IO_MODULES_COUNT)
		return RCS_MODULE_INVALID_ADDR;
	const char16_t* fw_utf16 = reinterpret_cast<const char16_t*>(QString("-").utf16());
	StrUtil::strcpy<char16_t>(fw_utf16, fw, fwLen);
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// General library configuration

extern "C" RCS_XN_SHARED_EXPORT unsigned int CALL_CONV GetModuleInputsCount(unsigned int module) {
	(void)module;
	return IO_MODULE_PIN_COUNT;
}

extern "C" RCS_XN_SHARED_EXPORT unsigned int CALL_CONV GetModuleOutputsCount(unsigned int module) {
	(void)module;
	return IO_MODULE_PIN_COUNT;
}

///////////////////////////////////////////////////////////////////////////////
// Events binders

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindBeforeOpen(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.beforeOpen, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindAfterOpen(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.afterOpen, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindBeforeClose(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.beforeClose, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindAfterClose(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.afterClose, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindBeforeStart(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.beforeStart, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindAfterStart(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.afterStart, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindBeforeStop(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.beforeStop, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindAfterStop(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.afterStop, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindOnError(StdErrorEvent f, void* data) {
	rx.events.bind(rx.events.onError, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindOnLog(StdLogEvent f, void* data) {
	rx.events.bind(rx.events.onLog, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindOnInputChanged(StdModuleChangeEvent f, void* data) {
	rx.events.bind(rx.events.onInputChanged, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindOnOutputChanged(StdModuleChangeEvent f, void* data) {
	rx.events.bind(rx.events.onOutputChanged, f, data);
}

extern "C" RCS_XN_SHARED_EXPORT void CALL_CONV BindOnScanned(StdNotifyEvent f, void* data) {
	rx.events.bind(rx.events.onScanned, f, data);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace RcsXn
