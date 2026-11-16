// Native operations for a plugin whose players build/race local copies of one map.
// Matchmaking and messages remain the existing lobby module's responsibility.
#pragma once
#include <string>
#include <vector>
namespace partystage {
bool Reset(const std::string& token);
std::string Snapshot();
bool SetSnapshot(const std::string& text);
bool OpenBuild(int round);
bool BuildReady();
std::vector<std::string> Offers();
std::vector<std::string> OfferIcons();
int Choose(int index);
std::string Capture();
bool Append(const std::string& item, bool local);
bool OpenRace(int round);
bool RaceReady();
bool StartRace();
bool SetRaceLimit(int seconds);
double RaceRemaining();
bool ReplayFinished(int player);
std::string FinishedReplay();
bool PrepareReplayGroup(const std::vector<std::string>& ids,const std::vector<std::string>& recordings);
bool ReplayReady();
bool StartReplay();
bool HoldReplay();
double ReplayClock();
double FinishTime(int player);
std::string Error();
void Frame();
void End();
void Stopped(int plugin);
std::string Pack(const std::string& text);
std::string Unpack(const std::string& text);
std::string Digest(const std::string& text);
}
