/**
 * @file VPT.cpp
 * @Author Lukas Koszegy (l.koszegy@gmail.com)
 * @date November, 2015
 * @brief Definition functions for VPTSensor
 */

#include <cassert>

#include "main.h"
#include "VPT.h"

using namespace std;
using Poco::AutoPtr;
using Poco::Logger;
using Poco::Runnable;
using Poco::Util::IniFileConfiguration;

#define VPT_DEFAULT_WAKEUP_TIME 15 /* seconds */

static const string vpt_ini_file(void)
{
	return string(MODULES_DIR) + string(MOD_VPT_SENSOR) + ".ini";
}

VPTSensor::VPTSensor(IOTMessage _msg, shared_ptr<Aggregator> _agg) :
	agg(_agg),
	log(Poco::Logger::get("Adaapp-VPT")),
	msg(_msg)
{
	AutoPtr<IniFileConfiguration> cfg;
	try {
		cfg = new IniFileConfiguration(vpt_ini_file());
	}
	catch (Poco::Exception& ex) {
		log.error("Exception with config file reading:\n" + ex.displayText());
		exit (EXIT_FAILURE);
	}
	setLoggingLevel(log, cfg); /* Set logging level from configuration file*/
	setLoggingChannel(log, cfg); /* Set where to log (console, file, etc.)*/

	msg.state = "data";
	msg.priority = MSG_PRIO_SENSOR;
	msg.offset = 0;
	tt = fillDeviceTable();
	json.reset(new JSONDevices);
	http_client.reset(new HTTPClient);
}

void VPTSensor::fetchAndSendMessage(map<long long int, VPTDevice>::iterator &device)
{
	try {
		pair<bool, Command> response;
		if (createMsg(device->second)) {
			log.information("VPT: Sending values to server");
			response = agg->sendData(msg);
			if (response.first) {
				parseCmdFromServer(response.second);
			}
		}
		else {
			log.error("Can't load new value of VPT sensor, send terminated");
		}
	}
	catch (Poco::Exception & exc) {
		log.error(exc.displayText());
	}
}

unsigned int VPTSensor::nextWakeup(void)
{
	unsigned int min_time = ~0;

	for (auto it = map_devices.begin(); it != map_devices.end(); it++) {
		VPTDevice &device = it->second;

		if (device.time_left < min_time)
			min_time = device.time_left;
	}

	return min_time;
}

/**
 * Main method of this thread.
 * Periodically send "data" messages with current pressure sensor value.
 *
 * Naturally end when quit_global_flag is set to true,
 * so the Adaapp is terminating.
 */
void VPTSensor::run(){
	unsigned int next_wakeup = 0;
	detectDevices();
	pairDevices();

	for (auto device = map_devices.begin(); device != map_devices.end(); device++)
		fetchAndSendMessage(device);

	while( !quit_global_flag ) {
		for (auto it = map_devices.begin(); it != map_devices.end(); it++) {
			VPTDevice &device = it->second;

			if (device.time_left == 0) {
				fetchAndSendMessage(it);
				device.time_left = device.wake_up_time;
			}

			assert(next_wakeup >= device.time_left);
			device.time_left -= next_wakeup;
		}

		next_wakeup = nextWakeup();

		for (unsigned int i = 0; i < next_wakeup; i++) {
			if (quit_global_flag)
				break;
			sleep(1);
		}
	}
}

#define VPT_ID_PREFIX 0xa4000000
#define VPT_ID_MASK   0x00ffffff

long long int VPTSensor::parseDeviceId(string &content)
{
	string id = json->getParameterValuesFromContent("id", content);
	long long int id32 = stoul(id.substr(5,11), nullptr, 16);
	return VPT_ID_PREFIX | (id32 & VPT_ID_MASK);
}

void VPTSensor::detectDevices(void) {
	log.information("VPT: Start device discovery");
	long long int id;
	vector<string> devices = http_client->discoverDevices();
	VPTDevice device;
	for ( vector<string>::iterator it = devices.begin(); it != devices.end(); it++ ) {
		try {
			string content = http_client->sendRequest(*it);
			id = parseDeviceId(content);
			device.name = json->getParameterValuesFromContent("device", content);
			device.ip = *it;
			device.sensor.version = 1;
			device.sensor.euid = id;
			device.sensor.device_id = json->getID(device.name);
			device.sensor.pairs = 0;
			device.sensor.values.clear();
			log.information("VPT: Detected device " + device.name + " with ip " + device.ip);
			map_devices.insert({id, device});
		}
		catch ( ... ) {/* exceptions are cought in the caller */}
	}
	log.information("VPT: Stop device discovery");
}

bool VPTSensor::isVPTSensor(long long int euid) {
	return map_devices.find(euid) != map_devices.end();
}

void VPTSensor::updateDeviceWakeUp(long long int euid, unsigned int time)
{
	auto it = map_devices.find(euid);
	if (it == map_devices.end()) {
		log.warning("VPT: Setting wake up on unknown device " + to_string(euid));
		return;
	}

	VPTDevice &dev = it->second;

	if ( time < VPT_DEFAULT_WAKEUP_TIME ) {
		dev.wake_up_time = VPT_DEFAULT_WAKEUP_TIME;
	}
	else {
		dev.wake_up_time = time;
	}

	dev.time_left = dev.wake_up_time;
}

void VPTSensor::processCmdSet(Command cmd)
{
	map<long long int, VPTDevice>::iterator it_ptr;
	if ( (it_ptr = map_devices.find(cmd.euid)) != map_devices.end() ) {
		pair<int, float> value = cmd.values.at(0);
		log.information("VPT: " + it_ptr->second.ip + ": Set actuator with ID:" + to_string(value.first) + " on " + to_string((int)value.second));

		string request_url = json->generateRequestURL(it_ptr->second.name, value.first, value.second);
		if (request_url.empty()) {
			log.error("VPT: Setting actuator failed - device or actuator not found");
			return;
		}

		http_client->sendRequest(it_ptr->second.ip, request_url);
	}
}

void VPTSensor::processCmdListen(void)
{
	try {
		detectDevices();
		pairDevices();
	}
	catch ( Poco::Exception & exc ) {
		log.error("VPT: " + exc.displayText());
	}
}

void VPTSensor::parseCmdFromServer(Command cmd){
	if (cmd.state == "update") {
		updateDeviceWakeUp(cmd.euid, cmd.time);
		return;
	}
	else if (cmd.state == "set") {
		processCmdSet(cmd);
		return;
	}
	else if (cmd.state == "listen") {
		processCmdListen();
		return;
	}
	log.error("Unexpected answer from server, received command: " + cmd.state);
}

bool VPTSensor::createMsg(VPTDevice &device) {
	string website = http_client->sendRequest(device.ip);

	try {
		device.sensor.values = json->getSensors(website);
	}
	catch ( Poco::Exception & exc ) {
		log.error("VPT: " + exc.displayText());
		return false;
	}
	device.sensor.pairs = device.sensor.values.size();
	device.sensor.device_id = json->getID(device.name);
	msg.device = device.sensor;
	msg.time = time(NULL);
	return true;
}

void VPTSensor::pairDevices(void) {
	for( auto device = map_devices.begin(); device != map_devices.end(); device++ ) {
		try {
			json->loadDeviceConfiguration(device->second.name);
		}
		catch (Poco::Exception &e) {
			log.error("VPT: " + e.displayText());
		}
	}
}
