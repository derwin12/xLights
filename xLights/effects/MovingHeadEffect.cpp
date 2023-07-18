/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/smeighan/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/smeighan/xLights/blob/master/License.txt
 **************************************************************/

#include "../../include/moving-head-16.xpm"
#include "../../include/moving-head-24.xpm"
#include "../../include/moving-head-32.xpm"
#include "../../include/moving-head-48.xpm"
#include "../../include/moving-head-64.xpm"

#include "MovingHeadEffect.h"
#include "MovingHeadPanel.h"
#include "../sequencer/Effect.h"
#include "../sequencer/Element.h"
#include "../sequencer/SequenceElements.h"
#include "../RenderBuffer.h"
#include "../UtilClasses.h"
#include "../UtilFunctions.h"
#include "../models/DMX/DmxMovingHeadAdv.h"
#include "../models/DMX/DmxMotor.h"
#include "../models/ModelGroup.h"

MovingHeadEffect::MovingHeadEffect(int id) : RenderableEffect(id, "Moving Head", moving_head_16, moving_head_24, moving_head_32, moving_head_48, moving_head_64)
{
    //ctor
}

MovingHeadEffect::~MovingHeadEffect()
{
    //dtor
}

xlEffectPanel *MovingHeadEffect::CreatePanel(wxWindow *parent) {
    return new MovingHeadPanel(parent);
}

std::list<std::string> MovingHeadEffect::CheckEffectSettings(const SettingsMap& settings, AudioManager* media, Model* model, Effect* eff, bool renderCache)
{
    std::list<std::string> res;

    return res;
}

void MovingHeadEffect::RenameTimingTrack(std::string oldname, std::string newname, Effect* effect)
{
    wxString timing = effect->GetSettings().Get("E_CHOICE_Servo_TimingTrack", "");

    if (timing.ToStdString() == oldname)
    {
        effect->GetSettings()["E_CHOICE_Servo_TimingTrack"] = wxString(newname);
    }
}

void MovingHeadEffect::SetDefaultParameters() {
    MovingHeadPanel *dp = (MovingHeadPanel*)panel;
    if (dp == nullptr) {
        return;
    }

    SetSliderValue(dp->Slider_MHPan, 0);
    SetSliderValue(dp->Slider_MHTilt, 0);
    SetSliderValue(dp->Slider_MHCycles, 10);

    dp->ValueCurve_MHPan->SetActive(false);
    dp->ValueCurve_MHTilt->SetActive(false);
    dp->ValueCurve_MHCycles->SetActive(false);
}

void MovingHeadEffect::Render(Effect *effect, const SettingsMap &SettingsMap, RenderBuffer &buffer) {
    double cycles = GetValueCurveDouble("MHCycles", 1.0, SettingsMap, 0.0f, MOVING_HEAD_CYCLES_MIN, MOVING_HEAD_CYCLES_MAX, buffer.GetStartTimeMS(), buffer.GetEndTimeMS(), MOVING_HEAD_DIVISOR);
    double eff_pos = buffer.GetEffectTimeIntervalPosition(cycles);

    if (buffer.cur_model == "") {
        return;
    }
    Model* model_info = buffer.GetModel();
    if (model_info == nullptr) {
        return;
    }

    const std::string& string_type = model_info->GetStringType();

    if (StartsWith(string_type, "Single Color")) {

        if( model_info->GetDisplayAs() == "DmxMovingHeadAdv" ) {
            MovingHeadPanel *p = (MovingHeadPanel*)panel;
            if (p == nullptr) {
                return;
            }

            int head_count = 0;
            for( int i = 1; i <= 8; ++i ) {
                wxString checkbox_ctrl = wxString::Format("IDD_CHECKBOX_MH%d", i);
                wxCheckBox* checkbox = (wxCheckBox*)(p->FindWindowByName(checkbox_ctrl));
                if( checkbox != nullptr ) {
                    if( checkbox->IsEnabled() ) {
                        head_count++;
                    }
                }
            }

            auto models = GetModels(model_info);

            for( int i = 1; i <= 8; ++i ) {
                wxString mh_textbox = wxString::Format("TEXTCTRL_MH%d_Settings", i);
                std::string mh_settings = SettingsMap[mh_textbox];
                if( mh_settings != "" ) {

                    // parse all the commands
                    float pan_pos = 0.0f;
                    float tilt_pos = 0.0f;
                    float pan_offset = 0.0f;
                    float tilt_offset = 0.0f;
                    float time_offset = 0.0f;
                    float path_scale = 0.0f;
                    float delta = 0.0f;
                    wxPoint2DDouble path_pt;
                    bool path_parsed = false;
                    bool pan_path_active = false;
                    bool tilt_path_active = false;
                    wxArrayString heads;
                    int groupings = 1;
                    wxArrayString all_cmds = wxSplit(mh_settings, ';');
                    for (size_t j = 0; j < all_cmds.size(); ++j )
                    {
                        std::string cmd = all_cmds[j];
                        int pos = cmd.find(":");
                        std::string cmd_type = cmd.substr(0, pos);
                        std::string settings = cmd.substr(pos+2, cmd.length());
                        std::replace( settings.begin(), settings.end(), '@', ';');

                        if( cmd_type == "Pan" ) {
                            pan_pos = atof(settings.c_str());
                        } else if ( cmd_type == "Tilt" ) {
                            tilt_pos = atof(settings.c_str());
                        } else if ( cmd_type == "Pan VC" ) {
                            GetValueCurvePosition(pan_pos, settings, eff_pos, buffer);
                        } else if ( cmd_type == "Tilt VC" ) {
                            GetValueCurvePosition(tilt_pos, settings, eff_pos, buffer);
                        } else if( cmd_type == "PanOffset" ) {
                            pan_offset = atof(settings.c_str());
                        } else if( cmd_type == "TiltOffset" ) {
                            tilt_offset = atof(settings.c_str());
                        } else if( cmd_type == ("PanOffset VC") ) {
                            GetValueCurvePosition(pan_offset, settings, eff_pos, buffer);
                        } else if( cmd_type == ("TiltOffset VC") ) {
                            GetValueCurvePosition(tilt_offset, settings, eff_pos, buffer);
                        } else if ( cmd_type == "Pan Path" ) {
                            pan_path_active = true;
                            if( !path_parsed ) {
                                GetPathPosition(path_pt, eff_pos, SettingsMap);
                                path_parsed = true;
                            }
                        } else if ( cmd_type == "Tilt Path" ) {
                            tilt_path_active = true;
                            if( !path_parsed ) {
                                GetPathPosition(path_pt, eff_pos, SettingsMap);
                                path_parsed = true;
                            }
                        } else if( cmd_type == "Heads" ) {
                            heads = wxSplit(settings, ',');
                        } else if( cmd_type == "Groupings" ) {
                            groupings = atoi(settings.c_str());
                        } else if( cmd_type == "Groupings VC" ) {
                            ValueCurve vc( settings );
                            vc.SetLimits(MOVING_HEAD_GROUP_MIN, MOVING_HEAD_GROUP_MAX);
                            groupings = vc.GetOutputValueAtDivided(eff_pos, buffer.GetStartTimeMS(), buffer.GetEndTimeMS());
                        } else if( cmd_type == "TimeOffset" ) {
                            time_offset = atof(settings.c_str());
                        } else if( cmd_type == "PathScale" ) {
                            path_scale = atof(settings.c_str());
                        } else if( cmd_type == "TimeOffset VC" ) {
                            ValueCurve vc( settings );
                            vc.SetLimits(MOVING_HEAD_TIME_MIN, MOVING_HEAD_TIME_MAX);
                            vc.SetDivisor(MOVING_HEAD_DIVISOR);
                            time_offset = vc.GetOutputValueAtDivided(eff_pos, buffer.GetStartTimeMS(), buffer.GetEndTimeMS());
                        } else if( cmd_type == "PathScale VC" ) {
                            ValueCurve vc( settings );
                            vc.SetLimits(MOVING_HEAD_SCALE_MIN, MOVING_HEAD_SCALE_MAX);
                            vc.SetDivisor(MOVING_HEAD_DIVISOR);
                            path_scale = vc.GetOutputValueAtDivided(eff_pos, buffer.GetStartTimeMS(), buffer.GetEndTimeMS());
                        }
                    }

                    CalculatePosition( i, pan_pos, heads, groupings, pan_offset, delta);
                    CalculatePosition( i, tilt_pos, heads, groupings, tilt_offset, delta);

                    if( path_parsed ) {
                        CalculatePathPositions( pan_path_active, tilt_path_active, pan_pos, tilt_pos, time_offset, path_scale, delta, eff_pos, SettingsMap);
                    }

                    // find models that map to this moving head position
                    for (const auto& it : models) {
                        if( it->GetDisplayAs() == "DmxMovingHeadAdv" ) {
                            DmxMovingHeadAdv* mhead = (DmxMovingHeadAdv*)it;
                            if( mhead->GetFixtureVal() == i ) {
                                int pan_cmd = (int)mhead->GetPanMotor()->ConvertPostoCmd(-pan_pos);
                                int tilt_cmd = (int)mhead->GetTiltMotor()->ConvertPostoCmd(-tilt_pos);
                                
                                WriteCmdToPixel(mhead->GetPanMotor(), pan_cmd, buffer);
                                WriteCmdToPixel(mhead->GetTiltMotor(), tilt_cmd, buffer);
                            }
                        }
                    }
                }
            }
        }
    }
}


void MovingHeadEffect::GetValueCurvePosition(float& position, const std::string& settings, double eff_pos, RenderBuffer &buffer)
{
    ValueCurve vc( settings );
    vc.SetLimits(MOVING_HEAD_MIN, MOVING_HEAD_MAX);
    vc.SetDivisor(MOVING_HEAD_DIVISOR);
    position = vc.GetOutputValueAtDivided(eff_pos, buffer.GetStartTimeMS(), buffer.GetEndTimeMS());
}

void MovingHeadEffect::GetPathPosition(wxPoint2DDouble& pt, double eff_pos, const SettingsMap &SettingsMap)
{
    std::string path_def = SettingsMap["TEXTCTRL_MHPathDef"];
    if( path_def != xlEMPTY_STRING ) {
        SketchEffectSketch sketch(SketchEffectSketch::SketchFromString(path_def));
        sketch.getProgressPosition(eff_pos, pt.m_x, pt.m_y);
    }
}

void MovingHeadEffect::CalculatePosition(int location, float& position, wxArrayString& heads, int groupings, float offset, float& delta )
{
    std::map<int, int> locations;
    for (size_t i = 0; i < heads.size(); ++i )
    {
        int head = wxAtoi(heads[i]);
        locations[head] = i+1;
    }

    // calculate the slot number within the group
    float slot = (float)locations[location];
    float center = (float)(groupings > 1 ? groupings : heads.size()) / 2.0f + 0.5;
    if( groupings > 1 ) {
        slot = (float)((locations[location]-1) % groupings + 1);
    }
    delta = slot - center;
    position = delta * offset + position;
    delta = slot - 1; // normalize to 0 to pass along for time_offset
}

void MovingHeadEffect::CalculatePathPositions(bool pan_path_active, bool tilt_path_active, float& pan_pos, float& tilt_pos, float time_offset, float path_scale, float delta, double eff_pos, const SettingsMap &SettingsMap)
{
    std::string path_def = SettingsMap["TEXTCTRL_MHPathDef"];
    if( path_def != xlEMPTY_STRING ) {
        SketchEffectSketch sketch(SketchEffectSketch::SketchFromString(path_def));
        wxPoint2DDouble pt;
        double progress_pos = eff_pos + ((delta * time_offset) / 100.0f);
        if( abs(progress_pos) > 1.0f ) {
            int prog1 = (int)(abs(progress_pos) * 100.0f);
            prog1 = prog1 % 100;
            progress_pos = (double)(prog1 / 100.0f);
        }
        sketch.getProgressPosition(progress_pos, pt.m_x, pt.m_y);
        glm::vec3 point;
        float scale = 180.0f;
        float new_scale = path_scale;
        if( new_scale >= 0.0f ) {
            new_scale += 1.0f;
        } else {
            new_scale = 1.0f / abs(new_scale);
        }
        point.x = (pt.m_x - 0.5f) * scale * new_scale;
        point.y = scale / 2.0f;
        point.z = (0.5f - pt.m_y) * scale * new_scale;

        glm::vec4 position = glm::vec4(point, 1.0);
        glm::mat4 rotationMatrixPan = glm::rotate(glm::mat4(1.0f), glm::radians(pan_pos), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 rotationMatrixTilt = glm::rotate(glm::mat4(1.0f), glm::radians(tilt_pos), glm::vec3(1.0f, 0.0f, 0.0f));
        glm::vec4 path_position = rotationMatrixPan * rotationMatrixTilt * position;
        
        // find angle from coordinates
        float new_pan = 0.0f;
        float new_tilt = 0.0f;
        
        if( abs(path_position.z) < 0.0001f ) {
            if( path_position.x > 0.0001f ) {
                new_pan = 90.0f;
            } else if( path_position.x < -0.0001f ) {
                new_pan = -90.0f;
            }
        } else if( abs(path_position.x) > 0.0001f ) {
            new_pan = atan2(path_position.x, path_position.z) * 180.0f / PI;
        }

        float hyp = sqrt(path_position.x * path_position.x + path_position.z * path_position.z);
        if( abs(path_position.y) < 0.0001f ) {
            if( path_position.z > 0.0001f ) {
                new_tilt = 90.0f;
            } else if( path_position.z < -0.0001f ) {
                new_tilt = -90.0f;
            }
        } else if( abs(hyp) > 0.0001f ) {
            new_tilt = atan2(hyp, path_position.y) * 180.0f / PI;
        }
        
        // adjust pan if pointed backwards
        if( new_pan < -90.0f ) {
            new_pan = 180.0f + new_pan;
            new_tilt = -new_tilt;
        }
        if( new_pan > 90.0f ) {
            new_pan = new_pan - 180.0f;
            new_tilt = -new_tilt;
        }
        
        if( pan_path_active ) {
            pan_pos = new_pan;
        }
        if( tilt_path_active ) {
            tilt_pos = new_tilt;
        }
    }

}

void MovingHeadEffect::WriteCmdToPixel(DmxMotor* motor, int value, RenderBuffer &buffer)
{
    xlColor lsb_c = xlBLACK;
    xlColor msb_c = xlBLACK;
    
    uint8_t lsb = value & 0xFF;
    uint8_t msb = value >> 8;
    lsb_c.red = lsb;
    lsb_c.green = lsb;
    lsb_c.blue = lsb;
    msb_c.red = msb;
    msb_c.green = msb;
    msb_c.blue = msb;
    int coarse_channel = motor->GetChannelCoarse() - 1;
    int fine_channel = motor->GetChannelFine() - 1;

    if( coarse_channel >= 0 ) {
        buffer.SetPixel(coarse_channel, 0, msb_c, false, false, true);
        if( fine_channel >= 0 ) {
            buffer.SetPixel(fine_channel, 0, lsb_c, false, false, true);
        }
    }
}

std::list<Model*> MovingHeadEffect::GetModels(Model* model)
{
    std::list<Model*> model_list;
    if (model != nullptr) {
        if (model->GetDisplayAs() == "ModelGroup") {
            auto mg = dynamic_cast<ModelGroup*>(model);
            if (mg != nullptr) {
                for (const auto& it : mg->GetFlatModels(true, false)) {
                    if (it->GetDisplayAs() != "ModelGroup" && it->GetDisplayAs() != "SubModel") {
                        model_list.push_back(it);
                    }
                }
            }
        }
        else if (model->GetDisplayAs() == "SubModel") {
            // don't add SubModels
        }
        else {
            model_list.push_back(model);
        }
    }

    return model_list;
}

void MovingHeadEffect::SetPanelStatus(Model *cls) {
    MovingHeadPanel *p = (MovingHeadPanel*)panel;
    if (p == nullptr) {
        return;
    }
    if (cls == nullptr) {
        return;
    }

    // disable all fixtures
    for( int i = 1; i <= 8; ++i ) {
        wxString checkbox_ctrl = wxString::Format("IDD_CHECKBOX_MH%d", i);
        wxCheckBox* checkbox = (wxCheckBox*)(p->FindWindowByName(checkbox_ctrl));
        if( checkbox != nullptr ) {
            checkbox->Enable(false);
            checkbox->SetValue(false);
        }
    }

    // find fixture numbers to enable
    auto models = GetModels(cls);
    bool single_model = models.size() == 1;
    for (const auto& it : models) {
        if( it->GetDisplayAs() == "DmxMovingHeadAdv" ) {
            DmxMovingHeadAdv* mhead = (DmxMovingHeadAdv*)it;
            wxString checkbox_ctrl = wxString::Format("IDD_CHECKBOX_MH%d", mhead->GetFixtureVal());
            wxCheckBox* checkbox = (wxCheckBox*)(p->FindWindowByName(checkbox_ctrl));
            if( checkbox != nullptr ) {
                checkbox->Enable(true);
                if( single_model ) {
                    checkbox->SetValue(true);
               }
            }
       }
    }

    p->FlexGridSizer_Main->Layout();
    p->Refresh();
}