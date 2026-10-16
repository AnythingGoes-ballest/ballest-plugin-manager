// Asset PoC: one ball whose material comes from this plugin's own cooked content (assets/, mounted by the host).
import bool AddBall(const string &in, const string &in, const string &in, const string &in, const string &in) from "cosmetic-kit";

bool added = false;

void Update(float dt)
{
    if (added) return;
    added = true;
    AddBall("asset-poc.shader", "PoC Shader", "", "", Plugins::Folder() + "models/poc.txt");
}
