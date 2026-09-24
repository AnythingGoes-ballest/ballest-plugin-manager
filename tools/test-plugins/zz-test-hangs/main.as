// Regression fixture: loops forever on its tenth frame. The host must stop it at its 20 ms budget.
int frames = 0;

void Update(float dt)
{
    if (++frames == 10)
        while (true) {}
}
