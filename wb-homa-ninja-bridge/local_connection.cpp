#include <iostream>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <getopt.h>
#include <memory>

#include "jsoncpp/json/json.h"
#include <mosquittopp.h>
#include <curl/curl.h>

#include "common/utils.h"
#include "common/mqtt_wrapper.h"
#include "http_helper.h"
#include "cloud_connection.h"
#include "local_connection.h"

using namespace std;

TMQTTNinjaBridgeHandler::TMQTTNinjaBridgeHandler(const TMQTTNinjaBridgeHandler::TConfig& mqtt_config)
    : TMQTTWrapper(mqtt_config)
    , VarDirectory("/var/lib/wirenboard/")
    , Activated(false)
    , ControlsCache()
{
    ReadBlockId();
    cout << "Ninja BlockId: " << BlockId << endl;

	Connect();
};




void TMQTTNinjaBridgeHandler::ReadBlockId()
{
    string serial_fname = VarDirectory + "serial.conf";
    ifstream serial_stream(serial_fname.c_str());

    if (not serial_stream.is_open()) {
        serial_stream.close();
        throw TBaseException("Cannot read serial from "  + serial_fname);
    }

    string wb_serial;
    serial_stream >> wb_serial;

    if (wb_serial.empty()) {
        throw TBaseException("Empty serial");
    }

    BlockId = string("Wb") + StringReplace(wb_serial,":","");
    StringUpper(BlockId);
}


string TMQTTNinjaBridgeHandler::GetTokenFilename() const
{
    return VarDirectory + BlockId + ".token";
}


bool TMQTTNinjaBridgeHandler::TryReadToken()
{
    const string & token_fname = GetTokenFilename();
    ifstream token_stream(token_fname.c_str());

    if (not token_stream.is_open()){
        cerr << "ERROR: Cannot read token from " << token_fname << endl;
        token_stream.close();
        return false;
    }

    token_stream >> Token;
    token_stream.close();

    if (not Token.empty()) {
        return true;
    }

    return false;
}

void TMQTTNinjaBridgeHandler::WriteToken()
{
    const string & token_fname = GetTokenFilename();
    ofstream token_stream(token_fname.c_str());

    if (not token_stream.is_open()){
        token_stream.close();
        throw TBaseException("Cannot write token to  " + token_fname);
    }

    token_stream << Token;
    token_stream.close();
}


bool TMQTTNinjaBridgeHandler::TryActivateBlock()
{

    string url = "http://wakai.ninja.is/rest/v0/block/";
    url += BlockId + "/activate";

	std::ostringstream oss;
    CURLcode rc = GetHttpUrl(url, oss, 10);
	if(rc == CURLE_OK) {
		std::string html = oss.str();
        cerr << "DEBUG: got response: " << html << endl;

        Json::Value root;
        Json::Reader reader;

        if(!reader.parse(html, root, false))  {
            cerr << "ERROR: Failed to parse Ninja activation response" << endl
               << reader.getFormatedErrorMessages() << endl;
            return false;
        }
        if (root.isMember("token")) {
            Token = root["token"].asString();
            cout << "Got token: " << Token << endl;
            return true;
        }

    }

    //~ cerr << "rc: " << rc << endl;
    return false;
}





int TMQTTNinjaBridgeHandler::PublishStatus(const string& status)
{
    return Publish(NULL, string("/devices/") + MQTTConfig.Id + "/controls/status", status, 0, true);
}

void TMQTTNinjaBridgeHandler::OnConnect(int rc)
{
	printf("Connected with code %d.\n", rc);

	if(rc == 0){
        string prefix = string("/devices/") + MQTTConfig.Id;
        Publish(NULL, prefix + "/meta/name", "NinjaBlocks bridge", 0, true);

        Publish(NULL, prefix + "/controls/block id", BlockId, 0, true);
        Publish(NULL, prefix + "/controls/block id/meta/type", "text", 0, true);
        Publish(NULL, prefix + "/controls/status/meta/type", "text", 0, true);
        PublishStatus("Waiting for activation");

        Subscribe(NULL, "/devices/+/controls/+");
        Subscribe(NULL, "/devices/+/controls/+/meta/type");

	}
}



void TMQTTNinjaBridgeHandler::WaitForActivation()
{
    if (Activated) return;

    if (not TryReadToken()) {
        cout << "Waiting for activation..." << endl;

        while (not TryActivateBlock()) {
            cerr << ".";
        }
        cerr << endl;

        cout << "Block is activated!" << endl;
        WriteToken();
    }

    Activated = true;

    cout << "Block token is " << Token << endl;
    PublishStatus("Activated");
}

void TMQTTNinjaBridgeHandler::ConnectToCloud()
{

    TMQTTWrapper::TConfig ninja_mqtt_config;
    ninja_mqtt_config.Host = "mqttbeta.ninjablocks.co";
    ninja_mqtt_config.Port = 8883;
    ninja_mqtt_config.Id =  BlockId;

    NinjaCloudMqttHandler.reset(new TMQTTNinjaCloudHandler(ninja_mqtt_config, this));

    //~ PublishStatus("Connected");

}


TControlDesc& TMQTTNinjaBridgeHandler::GetOrAddControlDesc(const string& device_id, const string& control_id)
{
    auto key = decltype(ControlsCache)::key_type(device_id, control_id);
    if (ControlsCache.find(key) == ControlsCache.end()) {
        // not found
        TControlDesc desc;
        desc.DeviceId = device_id;
        desc.Id = control_id;

        ControlsCache[key] = desc;
    }

    return ControlsCache[key];

}

void TMQTTNinjaBridgeHandler::OnMessage(const struct mosquitto_message *message)
{
    // ignore zero-length payload
    if (message->payload == 0) return;

    string topic = message->topic;
    string payload = static_cast<const char *>(message->payload);
    const vector<string>& tokens = StringSplit(topic, '/');
    bool match;

    if (mosquitto_topic_matches_sub("/devices/+/controls/+/meta/type", message->topic, &match) != MOSQ_ERR_SUCCESS) return;
    if (match) {
        const string& device_id = tokens[2];
        const string& control_id = tokens[4];

        TControlDesc & control = GetOrAddControlDesc(device_id, control_id);
        control.DeviceId = device_id;
        control.Id = control_id;
        control.Type = payload;
        control.Id = control_id;
    }

    if (mosquitto_topic_matches_sub("/devices/+/controls/+", message->topic, &match) != MOSQ_ERR_SUCCESS) return;
    if (match) {
        const string& device_id = tokens[2];
        const string& control_id = tokens[4];

        if (device_id != MQTTConfig.Id) {
            TControlDesc & control = GetOrAddControlDesc(device_id, control_id);
            control.Value = payload;
            //~ cout << "upd for " << control.DeviceId << ":" << control.Id << ":" << control.Type << "=" << control.Value << endl;


            if (NinjaCloudMqttHandler) {
                NinjaCloudMqttHandler->SendDeviceData(control);
            }
        }
    }
}


void TMQTTNinjaBridgeHandler::OnCloudControlUpdate(const string& device_id, const string& control_id, const string& value)
{
    auto iter = ControlsCache.find(make_pair(device_id, control_id));

    if (iter != ControlsCache.end()) {
        TControlDesc & control = iter->second;
        control.Value = value;

        Publish(NULL, "/devices/" + control.DeviceId + "/controls/" + control.Id + "/on", control.Value);
    }
}



void TMQTTNinjaBridgeHandler::OnSubscribe(int mid, int qos_count, const int *granted_qos)
{
}

//~ string TMQTTNinjaBridgeHandler::GetChannelTopic(const TSysfsOnewireDevice& device) {
    //~ static string controls_prefix = string("/devices/") + MQTTConfig.Id + "/controls/";
    //~ return (controls_prefix + device.GetDeviceId());
//~ }

//~ void TMQTTNinjaBridgeHandler::UpdateChannelValues() {
//~
    //~ for (const TSysfsOnewireDevice& device: Channels) {
        //~ auto result = device.ReadTemperature();
        //~ if (result.Defined()) {
            //~ Publish(NULL, GetChannelTopic(device), to_string(*result), 0, true); // Publish current value (make retained)
            //~ Publish(NULL, GetChannelTopic(device) + "/meta/type", "text", 0, true);
        //~ }
//~
    //~ }
//~ }




//~

