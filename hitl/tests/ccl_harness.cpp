// Decode a host->IG CIGI datagram with the CIGI Class Library, the way CamSim's
// receiver does (CIGI/CigiReceiver.cpp), and print the fields as key=value
// lines. Built and run by test_cigi.py::test_ccl_decodes_our_packets when the
// CCL sources (.build_tmp/ccl, from scripts/build_thirdparty.sh) and g++ exist.
//
//   ccl_harness <hex datagram>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "CigiIGSession.h"
#include "CigiIncomingMsg.h"
#include "CigiBaseEventProcessor.h"
#include "CigiIGCtrlV3_3.h"
#include "CigiEntityCtrlV3_3.h"
#include "CigiViewCtrlV3.h"
#include "CigiViewDefV3.h"
#include "CigiSensorCtrlV3.h"
#include "CigiHatHotReqV3_2.h"
#include "CigiWaveCtrlV3.h"

struct P : CigiBaseEventProcessor {
  void OnPacketReceived(CigiBasePacket *Packet) override {
    switch (Packet->GetPacketID()) {
      case 1: {
        auto *p = static_cast<CigiIGCtrlV3_3 *>(Packet);
        printf("ig.minor=%d\nig.frame=%u\nig.timestamp=%u\nig.ts_valid=%d\nig.mode=%d\nig.db=%d\n",
               (int)p->GetMinorVersion(), p->GetFrameCntr(), p->GetTimeStamp(), (int)p->GetTimeStampValid(),
               (int)p->GetIGMode(), (int)p->GetDatabaseID());
        break;
      }
      case 2: {
        auto *p = static_cast<CigiEntityCtrlV3_3 *>(Packet);
        printf("ent.id=%d\nent.state=%d\nent.attach=%d\nent.alpha=%d\nent.lat=%.10f\nent.lon=%.10f\nent.alt=%.6f\n"
               "ent.yaw=%.5f\nent.pitch=%.5f\nent.roll=%.5f\n",
               p->GetEntityID(), (int)p->GetEntityState(), (int)p->GetAttachState(), (int)p->GetAlpha(), p->GetLat(),
               p->GetLon(), p->GetAlt(), p->GetYaw(), p->GetPitch(), p->GetRoll());
        break;
      }
      case 16: {
        auto *p = static_cast<CigiViewCtrlV3 *>(Packet);
        printf("vc.view=%d\nvc.group=%d\nvc.entity=%d\nvc.yaw_en=%d\nvc.pitch_en=%d\nvc.roll_en=%d\nvc.x_en=%d\n"
               "vc.yaw=%.5f\nvc.pitch=%.5f\nvc.roll=%.5f\n",
               p->GetViewID(), (int)p->GetGroupID(), p->GetEntityID(), (int)p->GetYawEn(), (int)p->GetPitchEn(),
               (int)p->GetRollEn(), (int)p->GetXOffEn(), p->GetYaw(), p->GetPitch(), p->GetRoll());
        break;
      }
      case 21: {
        auto *p = static_cast<CigiViewDefV3 *>(Packet);
        printf("vd.view=%d\nvd.left=%.5f\nvd.right=%.5f\nvd.top=%.5f\nvd.bottom=%.5f\nvd.left_en=%d\nvd.bottom_en=%d\n",
               p->GetViewID(), p->GetFOVLeft(), p->GetFOVRight(), p->GetFOVTop(), p->GetFOVBottom(),
               (int)p->GetFOVLeftEn(), (int)p->GetFOVBottomEn());
        break;
      }
      case 17: {
        auto *p = static_cast<CigiSensorCtrlV3 *>(Packet);
        printf("sc.view=%d\nsc.sensor=%d\nsc.on=%d\nsc.polarity=%d\nsc.gain=%.3f\n", p->GetViewID(),
               (int)p->GetSensorID(), (int)p->GetSensorOn(), (int)p->GetPolarity(), p->GetGain());
        break;
      }
      case 24: {
        auto *p = static_cast<CigiHatHotReqV3_2 *>(Packet);
        printf("hh.id=%d\nhh.type=%d\nhh.coord=%d\nhh.lat=%.10f\nhh.lon=%.10f\nhh.alt=%.4f\n", p->GetHatHotID(),
               (int)p->GetReqType(), (int)p->GetSrcCoordSys(), p->GetLat(), p->GetLon(), p->GetAlt());
        break;
      }
      case 14: {
        auto *p = static_cast<CigiWaveCtrlV3 *>(Packet);
        printf("wv.en=%d\nwv.scope=%d\nwv.height=%.4f\nwv.len=%.4f\nwv.period=%.4f\nwv.dir=%.4f\n", (int)p->GetWaveEn(),
               (int)p->GetScope(), p->GetWaveHt(), p->GetWaveLen(), p->GetPeriod(), p->GetDirection());
        break;
      }
      default:
        printf("other=%d\n", Packet->GetPacketID());
    }
  }
};

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  std::string hex = argv[1];
  std::vector<Cigi_uint8> buf;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) buf.push_back((Cigi_uint8)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
  CigiIGSession session;
  session.SetCigiVersion(3, 3);
  CigiIncomingMsg &in = session.GetIncomingMsgMgr();
  in.SetReaderCigiVersion(3, 3);
  P proc;
  for (int id : {1, 2, 14, 16, 17, 21, 24}) in.RegisterEventProcessor(id, &proc);
  int r = in.ProcessIncomingMsg(buf.data(), (int)buf.size());
  printf("status=%d\n", r);
  return 0;
}
