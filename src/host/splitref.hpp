// A plugin's own split reference for one track: the checkpoint deltas under the race timer compare against it instead of
// the player's saved record. Read from the game (WBP_Split, AssetDumper pseudocode of /Game/UI/Gameplay/Elements/WBP_Split):
//   * the widget keeps its reference in PBSplits (Array<Double>, cumulative checkpoint times in seconds), filled once
//     when it is built (Construct -> CheckForRecord: the profile's record for the level, its CheckpointSplits) and
//     again after a finish that becomes the record (OnScoreGenerated / OnCustomLevelScoreGenerated);
//   * at every checkpoint the controller's RaceCheckpointSplitsUpdated(CurrentSplits) calls DisplayCheckpointSplit:
//     nothing is shown while PBSplits is empty; otherwise CalculateAndShowCheckpointSplit shows
//     |CurrentSplits[n-1] - PBSplits[n-1]| (TimeToText) with "+" (red) or "-" (green);
//   * at the finish UpdateRaceSplit shows the controller's own TrackSplitTimeText / TrackSplit+/- (the finish against
//     the saved best time).
// So the reference is put into PBSplits (and kept there: written again whenever the game changes it), read at the next
// checkpoint; an empty reference empties it (no deltas). At a finish on the track the finish delta is shown against
// the reference's total instead (or hidden without one). Only while race::CurrentTrack().key is the reference's key;
// nothing is saved anywhere (the widget's own array only). Game thread only.
#pragma once
#include <string>
#include <vector>

namespace splitref {

void Frame();
void Set(int owner, const std::string& trackKey, const std::vector<double>& splits, double total);
void Clear(int owner);
std::string Status();           // for tests: what the widget holds now and whether the reference applies

}  // namespace splitref
