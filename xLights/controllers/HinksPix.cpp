/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "HinksPix.h"
#include "ControllerCaps.h"
#include "../UtilFunctions.h"
#include "../models/Model.h"
#include "../models/ModelManager.h"
#include "../outputs/ControllerEthernet.h"
#include "../outputs/Output.h"
#include "../outputs/OutputManager.h"

#include "../xSchedule/wxJSON/jsonreader.h"
#include "../xSchedule/wxJSON/jsonwriter.h"

#include <wx/datetime.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sckipc.h>
#include "wx/socket.h"
#include <wx/sstream.h>

#ifdef _MSC_VER
#include <stdio.h>
#else
#include <unistd.h>
#endif

#include <log4cpp/Category.hh>
#include <filesystem>

#pragma pack(push, 2)

struct Tag_Packet {
    char HINK[18];
    uint8_t CMD[4];

    // struct Tag_TCP_Packet
    uint16_t TotalSize; // data and header
    uint16_t StructType; // allows any number of different message packets

    // struct Tag_File_Data
    uint16_t DataSize;
    uint8_t Data[580]; // must be even
};

// data follows or file name etc

struct Tag_TCP_Packet {
    uint16_t TotalSize; // data and header
    uint16_t StructType; // allows any number of different message packets
};

// CMD 'F' ******************
// StructType 0
struct Tag_File_Data_Start { // controller open temp file with hard write to overwrite previous faulted file
    uint16_t DataSize;
};

// StructType 1
struct Tag_File_Data { // controller open temp file with append
    uint16_t DataSize;
};

// StructType 2
struct Tag_File_Data_Close { // controller rename and set date/time
    // its part of packetword DataSize;	// so it matches earlier
    char FN[30]; // seq and playlist limit to 20 to 25
    uint32_t DTTM;
};

struct Tag_CMD_Packet {
    char HINK[18];
    uint8_t CMD[4];
};

struct Tag_Dow_TimePacket {
    uint8_t hr;
    uint8_t min;
    uint8_t sec;
    uint8_t dow;
};

#pragma pack(pop)



static size_t writeFunction(void* ptr, size_t size, size_t nmemb, std::string* data) {

    if (data == nullptr) return 0;
    data->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

#pragma region HinksPixOutput
void HinksPixOutput::Dump() const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("    Output %d Uni %d StartChan %d Pixels %d Dir %d Protocol %d Nulls %d ColorOrder %d Brightness %d Gamma %d ControlerStartChannel %d ControlerEndChannel %d Used %s",
                      output,
                      universe,
                      startChannel,
                      pixels,
                      direction,
                      protocol,
                      nullPixel,
                      colorOrder,
                      brightness,
                      gamma,
                      controllerStartChannel,
                      controllerEndChannel,
                      toStr(used));
}

void HinksPixOutput::SetConfig(wxString const& data) {
    const wxArrayString config = Split(data, std::vector<char>({ ',' }));
    if (config.size() != 10) {
        static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("Invalid config data '%s'", (const char*)data.c_str());
        return;
    }

    if (wxAtoi(config[0]) != output) {
        static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("Mismatched output ports data port:'%d' data:'%s'", output, (const char*)data.c_str());
        return;
    }
    if (config[1] != "undefined") {
        protocol = wxAtoi(config[1]);
    } else {
        protocol = 0;
    }
    controllerStartChannel = wxAtoi(config[2]);
    pixels = wxAtoi(config[3]);
    controllerEndChannel = wxAtoi(config[4]);
    direction = wxAtoi(config[5]);
    colorOrder = wxAtoi(config[6]);
    nullPixel = wxAtoi(config[7]);
    brightness = wxAtoi(config[8]);
    gamma = wxAtoi(config[9]);
}

wxString HinksPixOutput::BuildCommand() const {
    return wxString::Format("{\"V\":\"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\"}",
                            output, protocol, controllerStartChannel, pixels, controllerEndChannel,
                            direction, colorOrder, nullPixel, brightness, gamma);
}

wxString HinksPixOutput::BuildCommandEasyLights() const {

    return wxString::Format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d|",
        output, protocol, controllerStartChannel, pixels, controllerEndChannel,
        direction, colorOrder, nullPixel, brightness, gamma);
}

void HinksPixOutput::setControllerChannels(const int startChan) {
    controllerStartChannel = startChan;
    int chanPerPix = 3;                       //RGB nodes
    if (colorOrder == 6 || colorOrder == 7) { //RGBW nodes
        chanPerPix = 4;
    }
    controllerEndChannel = controllerStartChannel + (pixels * chanPerPix) - 1;
}
#pragma endregion

#pragma region HinksPixSerial
void HinksPixSerial::Dump() const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("   E131 Uni %d E131 StartChan %d E131 NumOfChan %d E131 Enabled %s DDP StartChan %d DDP NumOfChan %d DPP Enabled %s Upload %s",
                      e131Universe,
                      e131StartChannel,
                      e131NumOfChan,
                      toStr(e131Enabled),
                      ddpDMXStartChannel,
                      ddpDMXNumOfChan,
                      toStr(ddpDMXEnabled),
                      toStr(upload));
}

void HinksPixSerial::SetConfig(wxJSONValue const& data) {
    e131Enabled = data.ItemAt("DMX_ACTIVE").AsInt();
    e131Universe = data.ItemAt("DMX_UNIV").AsInt();
    e131StartChannel = data.ItemAt("DMX_START").AsInt();
    e131NumOfChan = data.ItemAt("DMX_CHAN_CNT").AsInt();

    ddpDMXEnabled = data.ItemAt("DDP_DMX_ACTIVE").AsInt();
    ddpDMXStartChannel = data.ItemAt("DDP_DMX_START").AsInt();
    ddpDMXNumOfChan = data.ItemAt("DDP_DMX_CHAN_CNT").AsInt();
}

wxString HinksPixSerial::BuildCommand() const {
    return wxString::Format("DATA: {\"CMD\":\"DATA_MODE\",\"DMX_ACTIVE\":%d,\"DMX_UNIV\":%d,\"DMX_START\":%d,\"DMX_CHAN_CNT\":%d,\"DDP_DMX_ACTIVE\":%d,\"DDP_DMX_START\":%d,\"DDP_DMX_CHAN_CNT\":%d}",
                            (int)e131Enabled, e131Universe, e131StartChannel, e131NumOfChan,
                            (int)ddpDMXEnabled, ddpDMXStartChannel, ddpDMXNumOfChan);
}

wxString HinksPixSerial::BuildCommandEasyLights(int mode) const
{
    return wxString::Format("A,%d,B,%d,C,%d,D,%d,E,%d,F,%d,G,%d,H,%d",
        mode, (int)e131Enabled, (int)ddpDMXEnabled, e131Universe, e131StartChannel,
        e131NumOfChan, ddpDMXStartChannel, ddpDMXNumOfChan);
}
#pragma endregion

#pragma region HinksPixSmart
void HinksSmartOutput::Dump() const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("  ID %d Type %d Port 1 Start Pixel %d Port 2 Start Pixel %d Port 3 Start Pixel %d Port 4 Start Pixel %d",
                      id,
                      type,
                      portStartPixel[0],
                      portStartPixel[1],
                      portStartPixel[2],
                      portStartPixel[3]);
}

void HinksSmartOutput::SetConfig(wxString const& data) {
    const wxArrayString config = Split(data, std::vector<char>({ ',' }));
    if (config.size() != 6) {
        static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("Invalid config data '%s'", (const char*)data.c_str());
        return;
    }

    id = wxAtoi(config[0]);
    type = wxAtoi(config[1]);
    portStartPixel[0] = wxAtoi(config[2]);
    portStartPixel[1] = wxAtoi(config[3]);
    portStartPixel[2] = wxAtoi(config[4]);
    portStartPixel[3] = wxAtoi(config[5]);
}

wxString HinksSmartOutput::BuildCommand() const {
    //{"V":"1,0,51,51,51,51"}
    return wxString::Format("{\"V\":\"%d,%d,%d,%d,%d,%d,\"}",
                            id, type, portStartPixel[0], portStartPixel[1],
                            portStartPixel[2], portStartPixel[3]);
}
#pragma endregion

#pragma region HinksPixInputUniverse
void HinksPixInputUniverse::Dump() const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("Index %d Uni %d UniSize %d HinksStart %d",
                      index,
                      universe,
                      numOfChan,
                      hinksPixStartChannel);
}

wxString HinksPixInputUniverse::BuildCommand() const {
    return wxString::Format("{\"V\":\"%d,%d,%d,1,%d,%d\"}", index,
                            universe, numOfChan, hinksPixStartChannel,
                            hinksPixStartChannel + numOfChan - 1);
}

wxString HinksPixInputUniverse::BuildCommandEasyLights() const {
    return wxString::Format("%d,%d,%d,1,%d,%d", index,
        universe, numOfChan, hinksPixStartChannel,
        hinksPixStartChannel + numOfChan - 1);
}
#pragma endregion

#pragma region Private Functions
bool HinksPix::InitControllerOutputData(bool fullControl, int defaultBrightness) {
    _pixelOutputs.clear();

    for (int i = 0; i < OUT_SIZE * EXP_PORTS; i++) {
        _pixelOutputs.push_back(HinksPixOutput(i + 1, defaultBrightness));
    }
    if (!fullControl) {
        for (int i = 0; i < EXP_PORTS; i++) {
            if (_EXP_Outputs[i] == EXPType::Local_SPI || _EXP_Outputs[i] == EXPType::Long_Range) {

                InitExpansionBoardData(i + 1, (i * OUT_SIZE) + 1, OUT_SIZE);
            }
        }
    }
    _serialOutput = InitSerialData(fullControl);
    return true;
}

void HinksPix::InitExpansionBoardData(int expansion, int startport, int length) {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    wxJSONValue data;

    bool const worked = GetControllerDataJSON(GetJSONPortURL(), data, wxString::Format("BLK: %d", expansion - 1));

    if (!worked || !data.HasMember("LIST")) {
        logger_base.error("Invalid Data from controller");
        return;
    }
    auto ports = data.ItemAt("LIST").AsArray();

    if (ports->Count() != length) {
        logger_base.error("Data from controller size and Expansion Size don't match");
        logger_base.error(data.ItemAt("LIST").AsString());
        return;
    }

    for (int i = 0; i < ports->Count(); i++) {
        auto stringValue = ports->Item(i)["V"].AsString();
        _pixelOutputs[(startport - 1) + i].SetConfig(stringValue);
    }
}

std::unique_ptr<HinksPixSerial> HinksPix::InitSerialData(bool fullControl) {
    std::unique_ptr<HinksPixSerial> serial = std::make_unique<HinksPixSerial>();
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    if (!fullControl) {

        if (_controllerType == "E") {
            logger_base.warn("Easylight 16 is missing Serial API ATM");
            return serial;
        }

        wxJSONValue data;
        bool const worked = GetControllerDataJSON(GetJSONModeURL(), data, "BLK: 0");

        if (!worked || !data.HasMember("CMD")) {
            logger_base.error("Invalid Data from controller");
            return serial;
        }
        if (data.Size() != 0) {
            serial->SetConfig(data);
        } else {
            static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
            logger_base.error("Invalid Return data %s", (const char*)data.AsString().c_str());
        }
    }

    return serial;
}

bool HinksPix::UploadInputUniverses(Controller* controller, std::vector<HinksPixInputUniverse> const& inputUniverses) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("HinksPix Inputs Upload: Uploading to %s", (const char*)_ip.c_str());

    if (_controllerType == "E")
    {
        return UploadInputUniversesEasyLights(controller, inputUniverses);
    }

    // Get universes based on IP
    std::list<Output*> outputs = controller->GetOutputs();

    if (controller->GetOutputCount() > _numberOfUniverses) {
        DisplayError(wxString::Format(
                         "Attempt to upload %d universes to HinksPix controller but only %d are supported.",
                         controller->GetOutputCount(), _numberOfUniverses)
                         .ToStdString());
        return false;
    }

    auto out = outputs.front();
    wxString type = "E131";
    wxString cmd;
    if (out->GetType() == OUTPUT_E131) {
        type = "E131";
        cmd = "DATA: {\"CMD\":\"DATA_MODE\",\"MODE\":\"E131\"}";
    } else if (out->GetType() == OUTPUT_ARTNET) {
        type = "ARTNET";
        cmd = "DATA: {\"CMD\":\"DATA_MODE\",\"MODE\":\"ARTNET\"}";
    } else if (out->GetType() == OUTPUT_DDP) {
        type = "DDP";
        cmd = wxString::Format("DATA: {\"CMD\":\"DATA_MODE\",\"MODE\":\"DDP\",\"DDP_START\":%d,\"DDP_CHAN_COUNT\":%d}",
                               out->GetStartChannel(), out->GetChannels());
    }

    wxJSONValue data;
    bool const worked = GetControllerDataJSON(GetJSONModeURL(), data, "BLK: 0");

    if (!worked || !data.HasMember("CMD")) {
        DisplayError("Getting HinksPix Input Mode FAILED.");
        return false;
    }

    //Set Controller Input mode
    //if (data.ItemAt("MODE").AsString() != type) //send mode every time
    {
        auto const ret = GetJSONControllerData(GetJSONPostURL(), cmd);
        if (ret.find("\"OK\"") == std::string::npos) {
            logger_base.error("Failed Return %s", (const char*)ret.c_str());
            DisplayError("Changing HinksPix Input Mode FAILED.");
            return false;
        }
    }

    //if DDP, skip the next part
    if (type == "DDP") {
        return true;
    }

    // Joes code allows 6 universe settings uploaded at a time
    //  loop though and submit every 6
    //
    //  EasyLights 16 max is 65 universes
    //  HinksPix Pro prev v111 firmware is 145 universes
    //  HinksPix Pro v111 firmware is 402 universes
    //  HinksPix Pro80 max is 684 universes
    int const numberOfCalls = std::ceil(_numberOfUniverses / UN_PER);

    int index = 1;
    int num_of_unv = 0;

    for (int j = 0; j < numberOfCalls; j++) {
        wxString requestString = wxString::Format("DATA: {\"CMD\":\"E131\",\"BLK\":\"%d\",\"LIST\":[", j);
        for (int i = 0; i < UN_PER; i++) {
            auto inpUn = std::find_if(inputUniverses.begin(), inputUniverses.end(), [index](auto const& inp) { return inp.index == index; });
            if (inpUn != inputUniverses.end()) {
                if (i != 0) {
                    requestString += ",";
                }
                requestString += inpUn->BuildCommand();
                index++;
                num_of_unv++;
            } else if (index <= _numberOfUniverses) {
                if (i != 0) {
                    requestString += ",";
                }
                requestString += wxString::Format("{\"V\":\"%d,%d,0,1,0,0\"}", index, index);
                index++;
            } else {
                requestString += "{\"V\":\"0,0,0,0,0,0\"}";
            }
        }
        requestString += "]}";
        //post data
        auto const ret = GetJSONControllerData(GetJSONPostURL(), requestString);
        if (ret.find("\"OK\"") == std::string::npos) {
            logger_base.error("Failed Return %s", (const char*)ret.c_str());
            return false;
        }
    }

    //set the universe count
    auto const unvrequestString = wxString::Format("DATA: {\"CMD\":\"BD_INFO\",\"NumU\":\"%d\"}", num_of_unv);
    auto const unvret = GetJSONControllerData(GetJSONPostURL(), unvrequestString);
    if (unvret.find("\"OK\"") == std::string::npos) {
        logger_base.error("Failed Return %s", (const char*)unvret.c_str());
        return false;
    }

    return true;
}



bool HinksPix::UploadUnPack(bool &worked, Controller *controller, std::vector<UnPack *> const &UPA, bool dirty) const
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    logger_base.debug("Building UnPack");
    wxString requestString;
    wxString LE;
    int BlkNum = 0;
    int LastIndex = 0;
    int Num2Send;
    int i, j;
    int TotalEntries;
    std::list<UnPack *> LL;

    for(i = 0; i < UPA.size(); i++)
    {
        if(UPA[i]->InActive == false)
            LL.push_back(UPA[i]);
    }

    if(LL.size() == 0 || dirty == false)
    {
        requestString = wxString::Format("DATA: {\"BLK\":\"%d\",\"NUM\":\"%d\",\"LEFT\":\"%d\",\"LIST\":[]}", 0, 0, 0);

        auto const ret = GetJSONControllerData(GetJSONUnPackURL(), requestString);
        if(ret.find("\"OK\"") == std::string::npos)
        {
            logger_base.error("Failed Return %s", (const char *)ret.c_str());
            worked = false;
            DisplayError("HinksPix UnPack FAILED.");

        }

        return worked;
    }

    TotalEntries = LL.size();
    LastIndex = -1;

    while(1)
    {
        // 16 is most can write to EE 256 size block

        Num2Send = (TotalEntries > 16) ? 16 : TotalEntries;
        j = 0;

        requestString = wxString::Format("DATA: {\"BLK\":\"%d\",\"NUM\":\"%d\",\"LEFT\":\"%d\",\"LIST\":[", BlkNum, Num2Send, (TotalEntries - Num2Send));

        i = 0;

        for(auto it = LL.begin(); it != LL.end(); ++it)
        {
            if(i > LastIndex)
            {
                LE = wxString::Format("{%d,%d,%d}", (*it)->MyStart, (*it)->NewStart, (*it)->NumChans);
                requestString += LE;

                LastIndex = i;
                j++;
                if(j >= Num2Send)
                    break;
            }

            i++;
        }

        requestString += "]}";

        auto const ret = GetJSONControllerData(GetJSONUnPackURL(), requestString);
        if(ret.find("\"OK\"") == std::string::npos)
        {
            logger_base.error("Failed Return %s", (const char *)ret.c_str());
            worked = false;
            DisplayError("HinksPix UnPack FAILED.");
            return worked;;
        }

        BlkNum++;
        TotalEntries -= Num2Send;

        if(TotalEntries <= 0)
            break;

    }

    return worked;
}


bool HinksPix::UploadInputUniversesEasyLights(Controller* controller, std::vector<HinksPixInputUniverse> const& inputUniverses) const
{
        static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.debug("HinksPix Inputs Upload: Uploading to %s", (const char*)_ip.c_str());

        auto const data = GetControllerData(902);
        if (data.empty()) {
            return false;
        }

        const auto map = StringToMap(data);

        int const maxUnv = wxAtoi(map.at("C"));

        if (controller->GetOutputCount() > maxUnv) {
            DisplayError(wxString::Format("Attempt to upload %d universes to HinksPix controller but only %d are supported.", controller->GetOutputCount(), maxUnv).ToStdString());
            return false;
        }

        auto out = controller->GetOutputs().front();
        int type = 0;//e131=0, ddp=1, artnet=2
        int multi = 0;
        int DDPStart = 0;
        if (out->GetType() == OUTPUT_E131) {
            type = 0;
        }
        else if (out->GetType() == OUTPUT_ARTNET) {
            type = 2;
        }
        else if (out->GetType() == OUTPUT_DDP) {
            type = 1;
            DDPStart = out->GetStartChannel();
        }

        if (out->GetIP() == "MULTICAST") {
            multi = 1;
        }

        bool worked = true;
        int num_of_unv = 0;

        //if DDP, skip the next part
        if (type != 1) {

            //Joes code allows 6 universe settings uploaded at a time
            // loop though and submit every 6
            // EasyLights 16 max is 65 universes
            int const numberOfCalls = std::ceil(maxUnv / UN_PER);
            int index = 1;

            for (int j = 0; j < numberOfCalls; j++) {
                wxString requestString = wxString::Format("ROWCNT=16:ROW=%d:", j);
                for (int i = 0; i < UN_PER; i++) {
                    auto inpUn = std::find_if(inputUniverses.begin(), inputUniverses.end(), [index](auto const& inp) { return inp.index == index; });
                    if (inpUn != inputUniverses.end()) {
                        if (i != 0) {
                            requestString += ",";
                        }
                        requestString += inpUn->BuildCommandEasyLights();
                        index++;
                        num_of_unv++;
                    }
                    else if (index <= maxUnv) {
                        if (i != 0) {
                            requestString += ",";
                        }
                        requestString += wxString::Format("%d,%d,0,1,0,0", index, index);
                        index++;
                    }
                    else {
                        requestString += ",0,0,0,0,0,0";
                    }
                }
                //post data
                auto const res = GetControllerData(2001, requestString);
                if (res != "done") {
                    worked = false;
                }
            }
        }

        auto const cmd = wxString::Format("A,%d,B,%d,C,%d,D,%d,E,%d",
            multi, type, maxUnv, num_of_unv, DDPStart);

        //Set Controller Input mode
        auto const setRet = GetControllerData(4902, cmd);
        if (setRet != "done")
        {
            logger_base.error("4902 Return %s", (const char*)setRet.c_str());
            worked = false;
        }

        if (!worked) {
            DisplayError("HinksPix E131 Input command FAILED.");
        }

        return worked;
}

void HinksPix::UploadPixelOutputsEasyLights(bool& worked)
{
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    logger_base.debug("Building pixel upload EasyLights");
    wxString requestString;

    for (int i = 0; i < 16; i++) {
        _pixelOutputs[i].Dump();
        requestString += _pixelOutputs[i].BuildCommandEasyLights();
    }
    requestString += "||";

    logger_base.info("Set String Output Information for EasyLights.");

    //Expansion Board "row" 'setting' commands are 3041, 3042, 3043 for expansion 1,2,3
    auto const pixelRet = GetControllerData(3041, requestString);
    if (pixelRet != "done") {
        logger_base.error("%d Return %s", 3041, (const char*)pixelRet.c_str());
        worked = false;
    }
}

void HinksPix::UploadPixelOutputs(bool& worked) const {
    for (int i = 0; i < EXP_PORTS; i++) {
        if (_EXP_Outputs[i] == EXPType::Local_SPI || _EXP_Outputs[i] == EXPType::Long_Range) {
            UploadExpansionBoardData(i + 1, (i * OUT_SIZE) + 1, OUT_SIZE, worked);
        }
    }
}

void HinksPix::UploadExpansionBoardData(int expansion, int startport, int length, bool& worked) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    logger_base.debug("Building pixel upload Expansion %d:", expansion);
    //{"CMD":"PCONFIG","BOARD":"0","LIST":[
    wxString requestString = wxString::Format("DATA: {\"CMD\":\"PCONFIG\",\"BOARD\":\"%d\",\"LIST\":[", expansion - 1);

    for (int i = 0; i < length; i++) {
        _pixelOutputs[(startport - 1) + i].Dump();
        requestString += _pixelOutputs[(startport - 1) + i].BuildCommand();
        requestString += ",";
    }

    requestString.RemoveLast(); //remove last ","
    requestString += "]}";

    auto const ret = GetJSONControllerData(GetJSONPostURL(), requestString);
    if (ret.find("\"OK\"") == std::string::npos) {
        logger_base.error("Failed Return %s", (const char*)ret.c_str());
        worked = false;
    }
}

void HinksPix::UpdatePortData(HinksPixOutput& pd, UDControllerPort* stringData, int32_t hinkstartChan) const {
    int const protocol = EncodeStringPortProtocol(stringData->GetFirstModel()->GetProtocol());
    if (protocol != -1) {
        pd.protocol = protocol;
    }

    int const nullPix = stringData->GetFirstModel()->GetStartNullPixels(-1);
    if (nullPix != -1) {
        pd.nullPixel = nullPix;
    }

    int const brightness = stringData->GetFirstModel()->GetBrightness(-1);
    if (brightness != -1) {
        pd.brightness = EncodeBrightness(brightness);
    }

    std::string const direction = stringData->GetFirstModel()->GetDirection("unknown");
    if (direction != "unknown") {
        pd.direction = EncodeDirection(direction);
    }

    int const gamma = stringData->GetFirstModel()->GetGamma(-1);
    if (gamma != -1) {
        pd.gamma = gamma;
    }

    std::string const color = stringData->GetFirstModel()->GetColourOrder("");
    if (!color.empty()) {
        pd.colorOrder = EncodeColorOrder(color);
    }
    pd.universe = stringData->GetUniverse();
    pd.startChannel = stringData->GetUniverseStartChannel();
    pd.pixels = stringData->Pixels();
    pd.used = true;
    pd.setControllerChannels(hinkstartChan);
}

void HinksPix::UpdateUniverseControllerChannels(UDControllerPort* stringData, std::vector<HinksPixInputUniverse>& inputUniverses, int32_t& hinkstartChan, int& index, bool individualUniverse) {
    if (individualUniverse) {
        for (auto const& m : stringData->GetModels()) {
            auto uni = m->GetUniverse();
            auto sizeofchan = m->Channels();

            while (sizeofchan > 0) {
                auto inpUn = std::find_if(inputUniverses.begin(), inputUniverses.end(), [uni](auto const& inp) { return inp.universe == uni; });
                if (inpUn != inputUniverses.end()) {
                    (*inpUn).index = index;
                    (*inpUn).hinksPixStartChannel = hinkstartChan;
                    sizeofchan -= (*inpUn).numOfChan;
                    uni++;
                    index++;
                    hinkstartChan += (*inpUn).numOfChan;
                }
            }
        }
    } else {
        for (auto const& m : stringData->GetModels()) {
            hinkstartChan += m->Channels();

        }
    }


}

void HinksPix::UpdateSerialData(HinksPixSerial& pd, UDControllerPort* serialData, int const mode, std::vector<HinksPixInputUniverse>& inputUniverses, int32_t& hinkstartChan, int& index, bool individualUniverse) const
{
    const int sc = serialData->GetStartChannel();
    const int usc = serialData->GetUniverseStartChannel();
    int maxChan = serialData->GetEndChannel() - sc + 1;

    if (maxChan < 16) {
        maxChan = 16;
    }

    if (mode != 1) { //0 and 2 are E131 and Artnet
        if (pd.e131Universe != (serialData->GetUniverse())) {
            pd.e131Universe = serialData->GetUniverse();
            pd.upload = true;
        }

        if (pd.e131StartChannel != usc) {
            pd.e131StartChannel = usc;
            pd.upload = true;
        }

        if (pd.e131NumOfChan != maxChan) {
            pd.e131NumOfChan = maxChan;
            pd.upload = true;
        }

        if (!pd.e131Enabled) {
            pd.e131Enabled = true;
            pd.upload = true;
        }

        if (individualUniverse) {
            auto const uni = serialData->GetUniverse();
            auto inpUn = std::find_if(inputUniverses.begin(), inputUniverses.end(), [uni](auto const& inp) { return inp.universe == uni; });
            if (inpUn != inputUniverses.end()) {
                (*inpUn).index = index;
                (*inpUn).hinksPixStartChannel = hinkstartChan;
                (*inpUn).numOfChan = maxChan;
                index++;
                hinkstartChan += maxChan;
            } 
        } else {
            hinkstartChan += maxChan;
        }
    } else { //1 is DDP
        if (pd.ddpDMXStartChannel != sc) {
            pd.ddpDMXStartChannel = sc;
            pd.upload = true;
        }
        if (pd.ddpDMXNumOfChan != maxChan) {
            pd.ddpDMXNumOfChan = maxChan;
            pd.upload = true;
        }

        if (!pd.ddpDMXEnabled) {
            pd.ddpDMXEnabled = true;
            pd.upload = true;
        }
    }
}

void HinksPix::UploadSmartReceivers(bool& worked) const {
    for (int exp = 0; exp < std::size(_smartOutputs); ++exp) {
        if (_EXP_Outputs[exp] != EXPType::Long_Range) {
            continue;
        }
        for (int bnk = 0; bnk < std::size(_smartOutputs[exp]); ++bnk) {
            UploadSmartReceiverData(exp, bnk, _smartOutputs[exp][bnk], worked);
        }
    }
}

void HinksPix::UploadSmartReceiverData(int expan, int bank, std::vector<HinksSmartOutput> const& receivers, bool& worked) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    logger_base.debug("Building SmartReciever upload Expansion %d Bank %d:", expan, bank);
    if (receivers.empty()) {
        logger_base.info("No SmartReciever found");
        return;
    }
    //{"CMD":"SCONFIG","BOARD":"0","Port4":"0","LIST":[{"V":"0,1,1,1,1,1"},{"V":"1,0,51,51,51,51"},{"V":"2,0,101,101,101,101"},{"V":"3,0,151,151,151,151"},{"V":"6,2,0,1,0,0"},{"V":"8,0,201,1,1,1"}]}
    wxString requestString = wxString::Format("DATA: {\"CMD\":\"SCONFIG\",\"BOARD\":\"%d\",\"Port4\":\"%d\",\"LIST\":[",
                                              expan, bank);

    for (auto const& rec: receivers) {
        rec.Dump();
        requestString += rec.BuildCommand();
        requestString += ",";
    }
    requestString.RemoveLast(); //remove last ","
    requestString += "]}";

    auto const ret = GetJSONControllerData(GetJSONPostURL(), requestString);
    if (ret.find("\"OK\"") == std::string::npos) {
        logger_base.error("Failed Return %s", (const char*)ret.c_str());
        worked = false;
    }
}

void HinksPix::CalculateSmartReceivers(UDControllerPort* stringData) {
    if (!stringData->AtLeastOneModelIsUsingSmartRemote()) {
        return;
    }
    int const port {stringData->GetPort() - 1};
    int const expansionBoard = port / OUT_SIZE;
    int const expansionPort = port % OUT_SIZE;
    int const bank = expansionPort / REC_SIZE;
    int const subPort = (expansionPort % REC_SIZE);

    int prevID { -1 };
    int start_pixels { 1 };
    int32_t portchans { 0 };
    for (const auto& it : stringData->GetModels()) {
        if (it->GetSmartRemote() > 0) {
            int id {it->GetSmartRemote() - 1};
            if (prevID != id) { //set data when going from A->B
                auto smartOut = std::find_if(_smartOutputs[expansionBoard][bank].begin(), _smartOutputs[expansionBoard][bank].end(), [id](auto const& so) {
                    return so.id == id;
                });
                if (smartOut != _smartOutputs[expansionBoard][bank].end()) {//if sr exists update start pixel count
                    smartOut->portStartPixel[subPort] = start_pixels;
                } else {//if sr doesn't exists, add it
                    if (it->GetSmartRemoteType().find("16") != std::string::npos && 
                        it->GetSmartRemoteType().find("16ac") == std::string::npos) { //add 16 port
                        //add all four remotes, starting at the first id 0,4,8,12
                        int const id16 = (id / REC_SIZE) * REC_SIZE;
                        auto& smartPort = _smartOutputs[expansionBoard][bank].emplace_back(id16);
                        smartPort.type = 1;
                        // fluff the Receivers
                        _smartOutputs[expansionBoard][bank].emplace_back(id16 + 1);
                        _smartOutputs[expansionBoard][bank].emplace_back(id16 + 2);
                        _smartOutputs[expansionBoard][bank].emplace_back(id16 + 3);
                        //get real sm 0,1,2,3, etc
                        auto smartOut16 = std::find_if(_smartOutputs[expansionBoard][bank].begin(), _smartOutputs[expansionBoard][bank].end(), [id](auto const& so) {
                            return so.id == id;
                        });
                        if (smartOut16 != _smartOutputs[expansionBoard][bank].end()) {//should always be true. we just added it
                            smartOut16->portStartPixel[subPort] = start_pixels;
                        }
                    } else {//add four port or 16ac
                        auto& smartPort = _smartOutputs[expansionBoard][bank].emplace_back(id);
                        smartPort.portStartPixel[subPort] = start_pixels;
                        if (it->GetSmartRemoteType().find("16ac") != std::string::npos) {
                            smartPort.type = 2;
                            smartPort.portStartPixel[subPort] = (portchans / 3) + 1;
                        }
                    }
                }
            }

            int32_t const chans = it->GetEndChannel() - it->GetStartChannel() + 1;
            portchans += chans;
            int const pixs = chans / std::max(it->GetChannelsPerPixel(), 3);
            start_pixels += pixs;
            prevID = id;
        }
    }
}

void HinksPix::SendRebootController(bool& worked) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    logger_base.debug("Sending Reboot Controller Command");
    PostToControllerNoResponse(GetJSONPostURL(), "DATA: {\"CMD\":\"OP_MODE\",\"MODE\":\"ETHERNET\"}");
}

//all of the Controller data is retrieved/set by "GET"ing different values
std::string HinksPix::GetJSONControllerData(std::string const& url, std::string const& data) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::string res;
    std::string const baseIP = _fppProxy.empty() ? _ip : _fppProxy;

    logger_base.debug("Making request to HinksPix '%s'.", (const char*)url.c_str());


    struct curl_slist* list = NULL;

    if (_curl) {
        curl_easy_setopt(_curl, CURLOPT_URL, std::string("http://" + baseIP + _baseUrl + url).c_str());
        curl_easy_setopt(_curl, CURLOPT_TIMEOUT, 20);

        list = curl_slist_append(list, "Content-type: text/plain");

        if (!data.empty()) {
            list = curl_slist_append(list, std::string(data).c_str());
            logger_base.debug("'%s'.", (const char*)data.c_str());
        }
        curl_easy_setopt(_curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(_curl, CURLOPT_MAXAGE_CONN, 120L);
        curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, list);

        std::string response_string;

        curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(_curl, CURLOPT_WRITEDATA, &response_string);

        curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        //curl_easy_setopt(_curl, CURLOPT_FORBID_REUSE, 1L);

        /* Perform the request, res will get the return code */
        CURLcode r = curl_easy_perform(_curl);

        if (r != CURLE_OK) {
            logger_base.error("Failure to access %s: %s.", (const char*)url.c_str(), curl_easy_strerror(r));
        } else {
            logger_base.debug("'%s'.", (const char*)response_string.c_str());
            res = response_string;
        }

        /* always cleanup */
        //curl_easy_cleanup(curl);
    } else {
        logger_base.error("Curl was null during HinksPix upload.");
    }
    return res;
}

//the reboot command reboots the controller with no response, not proper HTTP Request format but just timeout
void HinksPix::PostToControllerNoResponse(std::string const& url, std::string const& data) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::string res;
    std::string const baseIP = _fppProxy.empty() ? _ip : _fppProxy;

    logger_base.debug("Making request to HinksPix '%s'.", (const char*)url.c_str());

    struct curl_slist* list = NULL;

    if (_curl) {
        curl_easy_setopt(_curl, CURLOPT_URL, std::string("http://" + baseIP + _baseUrl + url).c_str());

        list = curl_slist_append(list, "Content-type: text/plain");

        if (!data.empty()) {
            list = curl_slist_append(list, std::string(data).c_str());
            logger_base.debug("'%s'.", (const char*)data.c_str());
        }

        curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(_curl, CURLOPT_TIMEOUT_MS, 1L); // just time out

        /* Perform the request, res will get the return code */
        curl_easy_perform(_curl);
        /* always cleanup */
       // curl_easy_cleanup(curl);
    } else {
        logger_base.error("Curl was null during HinksPix upload.");
    }
}

bool HinksPix::GetControllerDataJSON(const std::string& url, wxJSONValue& val, std::string const& data) const {
    std::string const sval = GetJSONControllerData(url, data);
    if (!sval.empty()) {
        wxJSONReader reader;
        reader.Parse(sval, &val);
        return true;
    }
    return false;
}
#pragma endregion

#pragma region Encode and Decode
int HinksPix::EncodeStringPortProtocol(const std::string& protocol) const {
    wxString p(protocol);
    p = p.Lower();

    if (p == "ws2811") {
        return 1;
    }
    if (p == "ws2812") {
        return 2; //unused
    }
    if (p == "ws2812b") {
        return 3; //unused
    }
    if (p == "ws2813") {
        return 4; //unused
    }
    if (p == "ws2801") {
        return 5;
    }
    if (p == "tls3001") {
        return 6;
    }
    if (p == "apa102") {
        return 7;
    }
    wxASSERT(false);
    return 1;
}

int HinksPix::EncodeColorOrder(const std::string& colorOrder) const {
    wxString c(colorOrder);
    c = c.Lower();

    if (c == "rgb") {
        return 0;
    }
    if (c == "rbg") {
        return 1;
    }
    if (c == "grb") {
        return 2;
    }
    if (c == "gbr") {
        return 3;
    }
    if (c == "brg") {
        return 4;
    }
    if (c == "bgr") {
        return 5;
    }
    if (c == "rgbw") {
        return 6;
    }
    if (c == "wrgb") {
        return 7;
    }
    wxASSERT(false);
    return 0;
}

int HinksPix::EncodeDirection(const std::string& direction) const {
    return direction == "Reverse";
}

int HinksPix::EncodeBrightness(int brightness) const {
    //HinkPix Allows brightness values of 100,90,80,70,60,50,40,30,20,15
    const int newVal = (brightness / 10) * 10; //round to the 10's
    if (newVal < 20) {
        return 15;
    }
    return newVal;
}

int HinksPix::EncodeGamma(int gamma) const {
    //HinkPix Allows gamma value of 1-4 as integer values
    if (gamma > 4) {
        return 4;
    }
    return gamma;
}

HinksPix::EXPType HinksPix::DecodeExpansionType(const std::string& type) const {
    if (type == "S") {
        return EXPType::Local_SPI;
    }
    if (type == "L") {
        return EXPType::Long_Range;
    }
    if (type == "A") {
        return EXPType::Local_AC;
    }
    return EXPType::Not_Present;
}

#pragma endregion

#pragma region Constructors and Destructors
HinksPix::HinksPix(const std::string& ip, const std::string& proxy) :
    BaseController(ip, proxy),
    _EXP_Outputs{ EXPType::Not_Present, EXPType::Not_Present, EXPType::Not_Present, EXPType::Not_Present, EXPType::Not_Present },
    _numberOfUniverses(0),
    _MCPU_Version(0) {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    //Get Controller Info
    wxJSONValue data;
    _curl = curl_easy_init();

    for (int x = 0; x < 3; x++) {
        if (GetControllerDataJSON(GetJSONInfoURL(), data, "")) {
            break;
        }
    }

    if (data.Size() > 0) {
        //get output type options

        for (int i = 0; i < EXP_PORTS; i++) {
            _EXP_Outputs[i] = DecodeExpansionType(data.Get(wxString::Format("BD%d",i + 1), "N").AsString());
        }        

        _connected = true;

        auto const pix_type = data.Get("Type", "").AsString();   //"P" for Pixel, "A" for AC
        _controllerType = data.Get("Controller", "").AsString(); //"H" for Pro, "E" for EasyLights

        _numberOfUniverses = wxAtoi(data.Get("MaxU", "0").AsString());

        _version = wxString::Format("MAIN:%s,POWER:%s,WIFI:%s,WEB:%s",
                                    data.Get("MCPU", "0").AsString().Mid(3),
                                    data.Get("PCPU", "0").AsString().Mid(3),
                                    data.Get("ECPU", "0").AsString().Mid(3),
                                    data.Get("WEB", "0").AsString().Mid(3));

        _MCPU_Version = wxAtoi(data.Get("MCPU", "0").AsString().Mid(3));

        if (_controllerType == "E") {
            _model = "EasyLights Pix16";
        } else if (_controllerType == "H") {
            if (pix_type == "8") {
                _model = "HinksPix PRO 80";
                _hardwareV3 = true;
            } else {
                _model = "HinksPix PRO";
            }
        } else {
            _model = "Unknown";
        }

        logger_base.debug("Connected to HinksPix controller model %s.", (const char*)GetFullName().c_str());
    } else {
        _connected = false;
        logger_base.error("Error connecting to HinksPix controller on %s.", (const char*)_ip.c_str());
        DisplayError(wxString::Format("Error connecting to HinksPix controller on %s.", _ip).ToStdString());
    }
}

HinksPix::~HinksPix() {
    _pixelOutputs.clear();
    if (_curl) {
        /* always cleanup */
        curl_easy_cleanup(_curl);
    }
}
#pragma endregion



struct less_than_key
{
    inline bool operator() (UnPack *A, UnPack *B)
    {
        return (A->MyStart < B->MyStart);
    }
};


#pragma region Getters and Setters
bool HinksPix::SetOutputs(ModelManager* allmodels, OutputManager* outputManager, Controller* c, wxWindow* parent) {
    ControllerEthernet* controller = dynamic_cast<ControllerEthernet*>(c);
    if (controller == nullptr) {
        DisplayError(wxString::Format("%s is not a HinksPix controller.", c->GetName().c_str()));
        return false;
    }

    if (_MCPU_Version < 101 && _controllerType == "H") {
        DisplayError(wxString::Format("HinksPix CPU Firmware is too old (v%d) Update to v101 or Newer.", _MCPU_Version));
        return false;
    }
    /*
    if (_MCPU_Version < 122 && _controllerType == "E") {
        DisplayError(wxString::Format("Easylights CPU Firmware is too old (v%d) Update to v122 or Newer.", _MCPU_Version));
        return false;
    }*/

    if (controller->GetModel() == "PRO V1/V2" && _model == "HinksPix PRO 80") {// Hinkle added 

        DisplayError(wxString::Format("Controller Reports as PRO V3 BUT You have the Model as PRO V1/V2 - Please Fix"));
        return false;
    }
    else if (controller->GetModel() == "PRO V3" && _model == "HinksPix PRO") {// Hinkle added 
        DisplayError(wxString::Format("Controller Reports as PRO V1/V2 BUT You have the Model as PRO V3 - Please Fix"));
        return false;
    }

    if(controller->IsUniversePerString() && IsUnPackSupported_Hinks(controller))
    {
        wxString msg = "HinksPix Requires LESS Universes with 'Universe Per String' disabled.  \r\nTurn OFF Universe Per String and Click SAVE.\r\n Then Check Number of Universes for this Controller - Should have Decreased.\r\n  Press YES to Change";
        if(wxMessageBox(msg, "Disable Universe Per String", wxYES_NO, parent) == wxYES)
        {
            return false;
        }
    }


    wxProgressDialog progress("Uploading ...", "", 100, parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE);
    progress.Show();

    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("HinksPix Outputs Upload: Uploading to %s", (const char*)_ip.c_str());

    // Get universes based on IP
    std::list<Output*> outputs = controller->GetOutputs();

    auto o = outputs.front();

    int mode = 0;

    if (o->GetType() == OUTPUT_E131) {
        mode = 0;
    }
    else if (o->GetType() == OUTPUT_ARTNET) {
        mode = 2;
    }
    else if (o->GetType() == OUTPUT_DDP) {
        mode = 1;

        if (controller->IsUniversePerString()) {
            DisplayError("HinksPix Upload Error:\nUniverse Per String not allows with DDP Output", parent);
            progress.Update(100, "Aborting.");
            return false;
        }
    }

    progress.Update(0, "Scanning models");
    logger_base.info("Scanning models.");

    std::string check;
    UDController cud(controller, outputManager, allmodels, false);

    //first check rules
    auto rules = ControllerCaps::GetControllerConfig(controller);
    const bool success = cud.Check(rules, check);
    logger_base.debug(check);

    cud.Dump();
    if (!success) {
        DisplayError("HinksPix Upload Error:\n" + check, parent);
        progress.Update(100, "Aborting.");
        return false;
    }

    bool const fullControl = rules->SupportsFullxLightsControl() && controller->IsFullxLightsControl();
    int const defaultBrightness = controller->GetDefaultBrightnessUnderFullControl();

    bool worked = true;

    logger_base.info("Initializing Pixel Output Information.");
    progress.Update(5, "Initializing Pixel Output Information.");

    InitControllerOutputData(fullControl, defaultBrightness);

    logger_base.info("Calculating Universe Start Channel Mappings.");
    progress.Update(10, "Calculating Universe Start Channel Mappings.");
    std::vector<HinksPixInputUniverse> inputUniverses;

    for (auto const& it : outputs) {
        inputUniverses.emplace_back(it->GetUniverse(), it->GetChannels());
    }

    if (!controller->IsUniversePerString() && o->GetType() != OUTPUT_DDP) {
        //old way
        int index = 1;
        int32_t startChan = 1;
        for (auto const& it : outputs) {
            auto uni = it->GetUniverse();
            auto inpUn = std::find_if(inputUniverses.begin(), inputUniverses.end(), [uni](auto const& inp) { return inp.universe == uni; });
            if (inpUn != inputUniverses.end()) {
                inpUn->index = index;
                inpUn->hinksPixStartChannel = startChan;
                index++;
                startChan += it->GetChannels();
            }
        }
    }

    logger_base.info("Figuring Out Pixel Output Information.");
    progress.Update(15, "Figuring Out Pixel Output Information.");
    int32_t hinkstartChan = 1;
    int univIdx = 1;
    //loop to setup string outputs
    for (int port = 1; port <= rules->GetMaxPixelPort(); port++) {
        if (cud.HasPixelPort(port)) {
            UDControllerPort* portData = cud.GetControllerPixelPort(port);
            auto pixOut = std::find_if(_pixelOutputs.begin(), _pixelOutputs.end(), [port](auto const& po) { return po.output == port; });
            if (pixOut != _pixelOutputs.end()) {
                UpdatePortData(*pixOut, portData, hinkstartChan);
                UpdateUniverseControllerChannels(portData, inputUniverses, hinkstartChan, univIdx, controller->IsUniversePerString());
            }
            CalculateSmartReceivers(portData);
        }
    }

    UnPack *UP;
    std::vector<UnPack *> UPA;
    int32_t HStart = -1;
    int32_t OffSet = 0;
    bool dirty = false;

    if(IsUnPackSupported_Hinks(controller))
    {
        if(controller->IsUniversePerString() == false)
        {
            for(int port = 1; port <= cud.GetMaxPixelPort(); port++)
            {
                if(cud.HasPixelPort(port))
                {
                    UDControllerPort *portData = cud.GetControllerPixelPort(port);
                    for(auto const &m : portData->GetModels())
                    {
                        if(HStart < 0)
                        {
                            HStart = 0;
                            OffSet = m->GetStartChannel() - 1;
                        }

                        UP = new UnPack;
                        UP->InActive = false;
                        UP->Port = port;
                        UP->NumChans = m->Channels();
                        UP->NewStart = HStart;
                        UP->NewEnd = UP->NewStart + UP->NumChans;
                        UP->MyStart = m->GetStartChannel() - 1 - OffSet;
                        UP->MyEnd = UP->MyStart + UP->NumChans;
                        HStart += UP->NumChans;
                        UPA.push_back(UP);
                    }
                }
            }

            if(cud.HasSerialPort(1))
            {
                UDControllerPort *portData = cud.GetControllerSerialPort(1);
                for(auto const &m : portData->GetModels())
                {
                    UP = new UnPack;
                    UP->InActive = false;
                    UP->Port = 100;
                    UP->NumChans = m->Channels();
                    UP->NewStart = HStart;
                    UP->NewEnd = UP->NewStart + UP->NumChans;
                    UP->MyStart = m->GetStartChannel() - 1 - OffSet;
                    UP->MyEnd = UP->MyStart + UP->NumChans;
                    HStart += UP->NumChans;
                    UPA.push_back(UP);

                }
            }


            logger_base.debug("Total Map\n");
            for(int i = 0; i < UPA.size(); i++)
            {
                logger_base.debug("%d %d  Port=%d MyStart=%d MyEnd=%d NewStart=%d NewEnd=%d NumChans=%d\n", i, UPA[i]->InActive, UPA[i]->Port, UPA[i]->MyStart, UPA[i]->MyEnd, UPA[i]->NewStart, UPA[i]->NewEnd, UPA[i]->NumChans);
            }
            logger_base.debug("\n\n\n");



            // sort by my start channel
            std::sort(UPA.begin(), UPA.end(), less_than_key());





            logger_base.debug("Total Map after sort before compress\n");
            for(int i = 0; i < UPA.size(); i++)
            {
                logger_base.debug("%d %d Port=%d MyStart=%d MyEnd=%d NewStart=%d NewEnd=%d NumChans=%d\n", i, UPA[i]->InActive, UPA[i]->Port, UPA[i]->MyStart, UPA[i]->MyEnd, UPA[i]->NewStart, UPA[i]->NewEnd, UPA[i]->NumChans);
            }
            logger_base.debug("\n\n\n");


            // combine/compress

            int LastUsed = 0;
            for(int i = 1; i < UPA.size(); i++)
            {
                if(UPA[i]->MyStart == UPA[i]->NewStart)  // we have continious memort
                {
                    UPA[LastUsed]->MyEnd += UPA[i]->NumChans;
                    UPA[LastUsed]->NewEnd += UPA[i]->NumChans;
                    UPA[LastUsed]->NumChans += UPA[i]->NumChans;
                    UPA[i]->InActive = true;
                    dirty = true;
                }
                else
                    LastUsed = i;

            }


                logger_base.debug("Total Map after compress and sort\n");
                for(int i = 0; i < UPA.size(); i++)
                {
                    logger_base.debug("%d %d Port=%d MyStart=%d MyEnd=%d NewStart=%d NewEnd=%d NumChans=%d\n", i, UPA[i]->InActive, UPA[i]->Port, UPA[i]->MyStart, UPA[i]->MyEnd, UPA[i]->NewStart, UPA[i]->NewEnd, UPA[i]->NumChans);
                }
                logger_base.debug("\n\n\n");
                logger_base.debug("Active only after compress and sort\n");
                for(int i = 0; i < UPA.size(); i++)
                {
                    if(UPA[i]->InActive == false)
                    {
                        logger_base.debug("%d %d Port=%d MyStart=%d MyEnd=%d NewStart=%d NewEnd=%d NumChans=%d\n", i, UPA[i]->InActive, UPA[i]->Port, UPA[i]->MyStart, UPA[i]->MyEnd, UPA[i]->NewStart, UPA[i]->NewEnd, UPA[i]->NumChans);
                    }
                }
                logger_base.debug("\n\n\n");


        }
    }


    logger_base.info("Checking Pixel Output and SmartReceivers Information.");
    progress.Update(20, "Checking Pixel Output and SmartReceivers Information.");

    if (!CheckPixelOutputs(check)) {
        DisplayError("HinksPix Upload Error:\n" + check, parent);
        progress.Update(100, "Aborting.");
        return false;
    }
    if (!CheckSmartReceivers(check)) {
        DisplayError("HinksPix Upload Error:\n" + check, parent);
        progress.Update(100, "Aborting.");
        return false;
    }

    logger_base.info("Figuring Out DMX Output Information.");
    progress.Update(25, "Figuring Out DMX Output Information.");

    if (cud.HasSerialPort(1)) {
        UDControllerPort* portData = cud.GetControllerSerialPort(1);
        UpdateSerialData(*_serialOutput, portData, mode, inputUniverses, hinkstartChan, univIdx, controller->IsUniversePerString());
    }

    logger_base.info("Uploading Input Universes Information.");
    progress.Update(30, "Uploading Input Universes Information.");
    worked &= UploadInputUniverses(controller, inputUniverses);

    logger_base.info("Uploading SmartReceivers Information.");
    progress.Update(60, "Uploading SmartReceivers Information.");
    UploadSmartReceivers(worked);

    logger_base.info("Uploading String Output Information.");
    progress.Update(70, "Uploading String Output Information.");
    if (_controllerType == "E") {
        UploadPixelOutputsEasyLights(worked);
    }
    else {
        UploadPixelOutputs(worked);
    }

    logger_base.info("Uploading DMX Output Information.");
    progress.Update(80, "Uploading DMX Output Information.");
    _serialOutput->Dump();

    if (_controllerType == "E") {
        if (cud.HasSerialPort(1)) {
            const std::string serialRequest = _serialOutput->BuildCommandEasyLights(mode);
            auto const serRet = GetControllerData(4908, serialRequest);
            if (serRet != "done") {
                logger_base.error("4908 Return %s", (const char*)serRet.c_str());
                worked = false;
            }
        }

        if(IsUnPackSupported_Hinks(controller))
        {
            logger_base.info("Uploading UnPack Information.");
            progress.Update(30, "Uploading UnPack Information.");
            UploadUnPack(worked, controller, UPA, dirty);
        }


        for(auto xx = UPA.begin(); xx != UPA.end(); ++xx)
            delete *xx;

        UPA.clear();

        //reboot
        logger_base.info("Rebooting Controller.");
        progress.Update(90, "Rebooting Controller.");
        auto const resetres = GetControllerData(1111);
        if (resetres != "done") {
            worked = false;
        }
    }
    else {
        if (_serialOutput->upload) {
            const std::string serialRequest = _serialOutput->BuildCommand();
            auto const ret = GetJSONControllerData(GetJSONPostURL(), serialRequest);
            if (ret.find("\"OK\"") == std::string::npos) {
                logger_base.error("Failed Return %s", (const char*)ret.c_str());
                worked = false;
            }
        }

        if(IsUnPackSupported_Hinks(controller))
        {
            logger_base.info("Uploading UnPack Information.");
            progress.Update(30, "Uploading UnPack Information.");
            UploadUnPack(worked, controller, UPA, dirty);
        }

        for(auto xx = UPA.begin(); xx != UPA.end(); ++xx)
            delete *xx;

        UPA.clear();

        //reboot
        logger_base.info("Rebooting Controller.");
        progress.Update(90, "Rebooting Controller.");
        SendRebootController(worked);
        wxMilliSleep(100);
        SendRebootController(worked);
    }

    progress.Update(100, "Done.");
    return worked;
}
#pragma endregion


//Get most data using a GET .cgi endpoint
wxString HinksPix::GetControllerData(int rowIndex, std::string const& data) const
{
    return GetControllerRowData(rowIndex, GetInfoURL(), data);
}

//E131 data uses a different .cgi endpoint
wxString HinksPix::GetControllerE131Data(int rowIndex) const
{
    return GetControllerRowData(rowIndex, GetE131URL(), std::string());
}

//all of the Controller data is retrieved/set by "GET"ing different ROW values
wxString HinksPix::GetControllerRowData(int rowIndex, std::string const& url, std::string const& data) const
{
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::string res;
    std::string const baseIP = _fppProxy.empty() ? _ip : _fppProxy;

    logger_base.debug("Making request to HinksPix '%s'.", (const char*)url.c_str());

    //CURL* curl = curl_easy_init();
    struct curl_slist* list = NULL;

    if (_curl) {
        curl_easy_setopt(_curl, CURLOPT_URL, std::string("http://" + baseIP + _baseUrl + url).c_str());
        curl_easy_setopt(_curl, CURLOPT_TIMEOUT, 20);

        list = curl_slist_append(list, "Content-type: text/plain");
        logger_base.debug("Row='%d'.", rowIndex);
        list = curl_slist_append(list, std::string("ROW: " + std::to_string(rowIndex)).c_str());

        if (!data.empty()) {
            list = curl_slist_append(list, std::string("DATA: " + data).c_str());
            logger_base.debug("DATA='%s'.", (const char*)data.c_str());
        }

        curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, list);

        std::string response_string;

        curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(_curl, CURLOPT_WRITEDATA, &response_string);

        curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L);
        //curl_easy_setopt(_curl, CURLOPT_FORBID_REUSE, 1L);

        /* Perform the request, res will get the return code */
        CURLcode r = curl_easy_perform(_curl);

        if (r != CURLE_OK) {
            logger_base.error("Failure to access %s: %s.", (const char*)url.c_str(), curl_easy_strerror(r));
        }
        else {
            res = response_string;
        }

        /* always cleanup */
        //curl_easy_cleanup(curl);
    }
    return res;
}

uint32_t GetDateTimeWord(int month, int day, int year, int hour, int min, int sec) 
{
    uint32_t const DT = (((year - 1980) * 512U) | month * 32U | day);
    uint32_t const TM = (hour * 2048U | min * 32U | sec / 2U);

    return ((DT << 16) & 0xffff0000) | (TM & 0xffff);
}

uint32_t GetDateTimeWord(wxDateTime dt)
{
    return GetDateTimeWord(dt.GetMonth() + 1, dt.GetDay(), dt.GetYear(), dt.GetHour(), dt.GetMinute(), dt.GetSecond());
}

bool ReadLineFromSocket(wxSocketClient* socket, std::string& line, long timeout) {
    line.clear();
    wxStopWatch sw;
    bool found{false};
    while ((timeout <= 0) || (sw.Time() < timeout)) {
        if (socket->WaitForRead(0, 1)) {
            char c;
            socket->Read(&c, sizeof(c)).GetLastIOReadSize();
            if (socket->LastCount() != sizeof(c) && found) {
                return true;
            }
            if (c == '|') {
                found = true;
               // line += c;
            }
            if (std::isprint(c)) {
                line += c;
            } else if (found) {
                return true;
            }
        }
        else {
           // wxYield(); // Allow other events to be processed
            // If not in a separate thread you might call wxYield() or wxSafeYield()
            // to prevent the app from hanging. But doing this can cause several
            // other problems which can be hard to debug, so be carefull!!
            if (found) {
                return true;
            }
        }
    }
    //int cc = sw.Time();
    return false; // Line not received as expected
}

bool HinksPix::UploadFileToController(std::string const& localpathname, std::string const& remotepathname, std::function<bool(int, int, std::string)> progress_dlg, wxDateTime const& fileTime) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::unique_ptr<wxSocketClient> sock = std::make_unique<wxSocketClient>();

    wxIPV4address addr;
    addr.Hostname(_ip);
    addr.Service(80);
    auto work = sock->Connect(addr);
    if (!work ) {
        logger_base.error("Could not connect to %s", (const char*)_ip.c_str());
        return false;
    }

    Tag_Packet PK;
    Tag_File_Data_Close DC;

    uint32_t NumBytes;
    int Progress = 0;

    int maxLoop = std::ceil(std::filesystem::file_size(localpathname) / sizeof(PK.Data)) + 1;

    auto up_message = wxString::Format("Uploading '%s' (%d/%d)", remotepathname, Progress, maxLoop);
    progress_dlg(Progress, maxLoop, up_message);

    FILE* f = fopen((const char*)localpathname.c_str(), "rb");
    if (f == NULL) {
        logger_base.error("Could not open file %s", (const char*)localpathname.c_str());
        sock->Close();
        return false;
    }
    memset(&PK, 0, sizeof(struct Tag_Packet));
    memmove(PK.HINK, "HINK TCP_CMD  \r\n\r\n", sizeof(PK.HINK)); // must be 18
    PK.CMD[0] = 'F';
    PK.CMD[1] = 0x5a;
    PK.CMD[2] = 0xa5;
    PK.CMD[3] = 0;
    PK.TotalSize = 0; // fix before xmit
    PK.StructType = 0;
    PK.DataSize = 0; // fix before xmit

    NumBytes = fread(PK.Data, 1, sizeof(PK.Data), f);
    PK.DataSize = (uint16_t)NumBytes;
    PK.TotalSize = sizeof(struct Tag_Packet) - sizeof(PK.Data) + (uint16_t)NumBytes;

    auto ss = sock->Write((uint8_t*)&PK, PK.TotalSize).LastCount();
    if (ss==0) {
        fclose(f);
        logger_base.error("ERROR Sending Data to Controller File Data");
        sock->Close();
        return false;
    }

    std::string line;
    ReadLineFromSocket(sock.get(), line, 5000);
    if (line.find("|FOK") == std::string::npos) {
        fclose(f);
        logger_base.error("Failed to Write %s", (const char*)line.c_str());
        sock->Close();
        return false;
    }
    wxStopWatch sw;
    while (true) 
    {
        Progress++;
        if (progress_dlg != nullptr) {
            auto time = sw.Time();
            wxString message;
            auto remaining = maxLoop - Progress;
            if (time > 0 && Progress > 0) {
                auto const rate = (time / Progress);
                auto const remainingTime = (int)(remaining * rate);
                auto const elapsed_seconds = remainingTime / 1000;
                auto const minutes = elapsed_seconds / 60;
                auto const seconds = elapsed_seconds % 60;
                message = wxString::Format("Uploading '%s' (%d/%d) Remaining time: %dm %ds", remotepathname, Progress, maxLoop, minutes, seconds);
            } else {
                message = wxString::Format("Uploading '%s' (%d/%d)", remotepathname, Progress, maxLoop);
            }
            auto con = progress_dlg(Progress, maxLoop+1, message);
            if (!con) {
                fclose(f);
                sock->Close();
                return false;
            }
        }

        if (NumBytes < sizeof(PK.Data)) // we have fully sent the file
        {
            fclose(f);
            // send closing info
            PK.StructType = 2;
            PK.DataSize = 0;
            memset(&DC, 0, sizeof(struct Tag_File_Data_Close));

            //strncpy(DC.FN, remotepathname, sizeof(DC.FN) - 1);
            std::strcpy(DC.FN, remotepathname.c_str());
            DC.DTTM = GetDateTimeWord(fileTime);
            memmove(PK.Data, &DC, sizeof(struct Tag_File_Data_Close));

            PK.TotalSize = sizeof(struct Tag_Packet) - sizeof(PK.Data) + sizeof(struct Tag_File_Data_Close);

            auto ss = sock->Write((uint8_t*)&PK, PK.TotalSize).LastCount();
            if (ss == 0) {
                sock->Close();
                return false;
            }

            ReadLineFromSocket(sock.get(), line, 5000);
            if (line.find("|FOK") == std::string::npos) {
                logger_base.error("Failed to Write %s", (const char*)line.c_str());
                sock->Close();
                return false;
            } else {
                logger_base.debug("File %s uploaded successfully", (const char*)remotepathname.c_str());
            } 
            sock->Close();
            return true;
        }

        // we are sending all the data here but the first one
        PK.StructType = 1;

        NumBytes = fread(PK.Data, 1, sizeof(PK.Data), f);
        PK.DataSize = (uint16_t)NumBytes;
        PK.TotalSize = sizeof(struct Tag_Packet) - sizeof(PK.Data) + (uint16_t)NumBytes;

        auto ss = sock->Write((uint8_t*)&PK, PK.TotalSize).LastCount();
        if (ss == 0) {
            fclose(f);
            logger_base.error("ERROR Xmitting to Controller File Data");
            sock->Close();
            return false;
        }

        ReadLineFromSocket(sock.get(), line, 5000);
        if (line.find("|FOK") == std::string::npos) {
            logger_base.error("Failed to Write %s", (const char*)line.c_str());
            fclose(f);
            sock->Close();
            return false;
        }

    }
}

bool HinksPix::UploadTimeToController() const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::unique_ptr<wxSocketClient> sock = std::make_unique<wxSocketClient>();

    wxIPV4address addr;
    addr.Hostname(_ip);
    addr.Service(80);
    auto work = sock->Connect(addr);
    if (!work) {
        logger_base.error("Could not connect to %s", (const char*)_ip.c_str());
        return false;
    }
    auto time = wxDateTime::Now();
    Tag_Dow_TimePacket TP;
    memset(&TP, 0, sizeof(struct Tag_Dow_TimePacket));
    TP.hr = time.GetHour();
    TP.min = time.GetMinute();
    TP.sec = time.GetSecond();
    TP.dow = time.GetWeekDay(); // zero based

    Tag_Packet PK;
    memset(&PK, 0, sizeof(struct Tag_Packet));
    memmove(PK.HINK, "HINK TCP_CMD  \r\n\r\n", sizeof(PK.HINK)); // must be 18
    PK.CMD[0] = 'D';
    PK.CMD[1] = 0x5a;
    PK.CMD[2] = 0xa5;
    PK.CMD[3] = 0;

    PK.TotalSize = sizeof(struct Tag_Packet) - sizeof(PK.Data) + sizeof(struct Tag_Dow_TimePacket);
    PK.StructType = 0;
    PK.DataSize = sizeof(struct Tag_Dow_TimePacket);

    memmove(PK.Data, &TP, sizeof(struct Tag_Dow_TimePacket));
    auto ss = sock->Write((uint8_t*)&PK, PK.TotalSize).LastCount();
    if (ss == 0) {
        logger_base.error("ERROR Sending Data to Controller File Data");
        sock->Close();
        return false;
    }

    std::string line;
    ReadLineFromSocket(sock.get(), line, 5000);
    if (line.find("|FOK") == std::string::npos) {
        logger_base.error("Failed to Write %s", (const char*)line.c_str());
        sock->Close();
        return false;
    }
    sock->Close();
    return true;
}

bool HinksPix::UploadModeToController(unsigned char mode) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::unique_ptr<wxSocketClient> sock = std::make_unique<wxSocketClient>();

    wxIPV4address addr;
    addr.Hostname(_ip);
    addr.Service(80);
    auto work = sock->Connect(addr);
    if (!work) {
        logger_base.error("Could not connect to %s", (const char*)_ip.c_str());
        return false;
    }

    Tag_CMD_Packet CP;
    memset(&CP, 0, sizeof(struct Tag_CMD_Packet));
    memmove(CP.HINK, "HINK TCP_CMD  \r\n\r\n", sizeof(CP.HINK)); // must be 18
    CP.CMD[0] = mode;
    CP.CMD[1] = 0x5a;
    CP.CMD[2] = 0xa5;
    CP.CMD[3] = 0;

    auto ss = sock->Write((uint8_t*)&CP, sizeof(struct Tag_CMD_Packet)).LastCount();
    if (ss == 0) {
        logger_base.error("ERROR Sending Mode to Controller");
        sock->Close();
        return false;
    }

    std::string line;
    ReadLineFromSocket(sock.get(), line, 5000);
    if (line.find("|FOK") == std::string::npos) {
        logger_base.error("Failed to Send %s", (const char*)line.c_str());
        sock->Close();
        return false;
    }
    sock->Close();
    return true;
}

std::vector<HinksPixFileData> HinksPix::GetFileInfoFromSDCard(uint8_t cmd) const {
    static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::unique_ptr<wxSocketClient> sock = std::make_unique<wxSocketClient>();
    std::vector<HinksPixFileData> files;
    uint8_t CMD[4];
    uint8_t B[100];
    char* p;
    int CmdLength;

    CMD[0] = cmd;
    CMD[1] = 0x5a;
    CMD[2] = 0xa5;
    CMD[3] = 0;

    wxIPV4address addr;
    // addr.AnyAddress();
    addr.Hostname(_ip);
    addr.Service(80);

    auto connected = sock->Connect(addr);
    if (!connected) {
        logger_base.error("Could not connect to %s", (const char*)_ip.c_str());
        return files;
    }

    snprintf((char*)B, sizeof(B), "HINK TCP_CMD  \r\n\r\n"); // we must fake a http header
    CmdLength = strlen((char*)B);
    p = (char*)B;
    while (*p)
        p++;
    memmove(p, CMD, 4);
    CmdLength += 4;

    auto ss = sock->Write(B, CmdLength).LastCount();
    if (ss == 0) {
        //fclose(f);
        sock->Close();
        logger_base.error("ERROR Xmitting to Controller File Data");
        return files;
    }

    //std::string line;
    // wxStopWatch sw;
    std::string data;
    ReadLineFromSocket(sock.get(), data, 5000);
    if (data.find("|FOK") != std::string::npos) {
        logger_base.error("Failed to Write %s", (const char*)data.c_str());
        // txtRx->AppendText(wxT("Failed to read message.\n"));
        sock->Close();
        return files;
    }
    //*FILENAME.HSEQ,1002,1504!
    auto const sfiles = Split(data, std::vector<char>({ '!' }));
    for (auto it : sfiles) {
        if (it.size() > 0) {
            HinksPixFileData file;
            if (it.starts_with('*')) {
                it = it.Remove(0, 1);
            }
            auto const parts = Split(it, std::vector<char>({ ',' }));
            if (parts.size() == 3) {
                file.FileName = parts[0];
                file.Date = wxAtoi(parts[1]);
                file.Time = wxAtoi(parts[2]);
                files.push_back(file);
            }
        }
    }
    sock->Close();
    return files;
}

bool HinksPix::FirmwareSupportsUpload() const {
    constexpr int V3UPLOADFIRMEWARE = 129;
    constexpr int V2UPLOADFIRMEWARE = 151;

    if (_controllerType == "E") {
        return false;
    }
    if (_hardwareV3) {
        if (_MCPU_Version >= V3UPLOADFIRMEWARE) {
            return true;
        }
    } else {    
        if (_MCPU_Version >= V2UPLOADFIRMEWARE) {
            return true;
        }
    }    
    return false;
}

std::map<wxString, wxString> HinksPix::StringToMap(wxString const& text) const
{
    std::map<wxString, wxString> map;
    const wxArrayString items = Split(text, std::vector<char>({ ',' }));
    if (items.size() % 2 == 0) {
        for (int i = 0; i < items.size() - 1; i += 2) {
            map[items[i]] = items[i + 1];
        }
    }
    return map;
}

bool HinksPix::CheckPixelOutputs(std::string& message)
{
    for (auto const& it : _pixelOutputs) {
        if (it.used) {
            int exp_idx = (it.output - 1) / OUT_SIZE;
            if (_EXP_Outputs[exp_idx] == EXPType::Not_Present) {
                message = "Pixel Output " + std::to_string(it.output) + " is being used on Expansion Port " + std::to_string(exp_idx + 1) + " and no Output Board is Connected!";
                return false;
            }
        }
    }

    return true;
}

bool HinksPix::CheckSmartReceivers(std::string& message)
{
    wxASSERT(std::size(_EXP_Outputs) == std::size(_smartOutputs));

    for (int exp = 0; exp < EXP_PORTS; ++exp) {
        if (_EXP_Outputs[exp] != EXPType::Long_Range) {
            for (int bnk = 0; bnk < std::size(_smartOutputs[exp]); ++bnk) {
                if (_smartOutputs[exp][bnk].size() != 0) {
                    message = "Expansion Port " + std::to_string(exp + 1) + " has Smart Receivers but it is not a Long Range Differential Board!";
                    return false;
                }
            }
        }
    }
    return true;
}


bool HinksPix::IsUnPackSupported_Hinks(ControllerEthernet *controller)
{
    std::string M = controller->GetModel();

    if(controller->GetModel() == "PRO V1/V2")
    {
        if(_MCPU_Version < 151)
            return false;

        return true;
    }
    else if(controller->GetModel() == "PRO V3")
    {
        if(_MCPU_Version < 129)
            return false;

        return true;
    }

    return false;
}
