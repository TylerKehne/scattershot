#include <Scattershot.hpp>
#include <Utils.hpp>

#define CONT_A 0x8000
#define CONT_B 0x4000
#define CONT_Z 0x2000
#define CONT_CRIGHT 0x0001
#define CONT_CLEFT 0x0002
#define CONT_CDOWN 0x0004
#define CONT_CUP 0x0008
#define CONT_START 0x1000
#define CONT_L 0x0020
#define CONT_DRIGHT 0x0100
#define CONT_DLEFT 0x0200
#define CONT_DDOWN 0x0400
#define CONT_DUP 0x0800

#define ACT_DIVE 0x08A
#define ACT_DIVE_LAND 0x056
#define ACT_DR 0x0A6
#define ACT_DR_LAND 0x032
#define ACT_WALK 0x040
#define ACT_BRAKE 0x045
#define ACT_TURNAROUND_1 0x043
#define ACT_TURNAROUND_2 0x044
#define ACT_FREEFALL_LAND 0x071
#define ACT_FREEFALL 0x08c

//care about these:
//air dive 08A
//ground dive = dive landing, 056
//forward rollout 0A6
//freefall landing (2/2) is dr landing. 032
//040 is walking/running, pre/tiptoeing
//045 is braking/skidding
//043 is turning around (1/2)
//044 is turning around (2/2)
//071 is freefall landing (1/2) for nut spot
//08c is freefall

//compiling for optimized run:
//gcc -Wall -O3 -fopenmp -o bob_island_multspeeds.exe bob_island_multspeeds.c -ldbghelp
//compiling for debug with gdb:
//gcc -Wall -fopenmp -g -o bob_island_multspeeds.exe bob_island_multspeeds.c -ldbghelp
//then
//gdb ./bob_island_multspeeds.exe

//useful gdb commands:
//thread apply all bt
//info threads
//bt
//bt full
//f 9 (or whatever stack frame depth you want)
//p curSeg (or whatever variable)

//look at threads 1, 6, 7, 8, 9, 10, 12

















//fifd: Where new inputs to try are actually produced
//I think this is a perturbation of the previous frame's input to be used for the upcoming frame
void perturbInput(HMODULE& dll, Input* in, uint64_t* seed, int frame, int megaRandom) {
    void* gMarioStates = GetProcAddress(dll, "gMarioStates");
    void* gObjectPool = GetProcAddress(dll, "gObjectPool");
    void* gCamera = GetProcAddress(dll, "gCamera");

    float* x = (float*)((char*)gMarioStates + 60);
    float* y = (float*)((char*)gMarioStates + 64);
    float* z = (float*)((char*)gMarioStates + 68);
    unsigned int* marioAction = (unsigned int*)((char*)gMarioStates + 12);
    uint16_t* marioYawFacing = (uint16_t*)((char*)gMarioStates + 46);
    float* marioHSpd = (float*)((char*)gMarioStates + 0x54);
    uint16_t* camYaw = (uint16_t*)((char*)gCamera + 340);

    float* pyraXNorm = (float*)((char*)gObjectPool + 84 * 1392 + 324);
    float* pyraZNorm = (float*)((char*)gObjectPool + 84 * 1392 + 332);

    if (frame == 0) in->x = in->y = in->b = 0;

    if ((in->b & CONT_DDOWN) != 0) { //on first frame of pause buffer
        in->x = in->y = in->b = 0;
        in->b |= CONT_DLEFT;  //mark that we are on second frame
        return;
    }
    if ((in->b & CONT_DLEFT) != 0) { //on second frame of pause buffer
        in->x = in->y = in->b = 0;
        in->b |= CONT_DUP;  //mark that we are on the third frame
        in->b |= CONT_START;  //unpause
        return;
    }
    if ((in->b & CONT_DUP) != 0) {  //on third frame of pause buffer
        in->x = in->y = in->b = 0; //wait for unpause to happen
        return;
    }

    //fifd: CONT_A and similar are bitmasks to identify buttons.
    //so doA is set to whether A is pressed in "in"
    //c is specifically c^
    //int doA = (in->b & CONT_A) != 0;
    //int doB = (in->b & CONT_B) != 0;
    //int doZ = (in->b & CONT_G) != 0;
    //int doC = (in->b & CONT_E) != 0;

    unsigned int actTrunc = *marioAction & 0x1FF;

    //fifd: Inverses of probabilities with which we toggle button statuses
    //or, in the case of jFact, joystick inputs
    int jFact = 5;
    //int aFact = 4;
    //int bFact = 15;
    //int zFact = 15;
    if (megaRandom) {
        jFact = 2;
        //aFact = 4;
        //bFact = 4;
        //zFact = 4;
    }
    if (actTrunc == ACT_TURNAROUND_1 || actTrunc == ACT_TURNAROUND_2 || actTrunc == ACT_BRAKE) { jFact *= 5; }

    if (actTrunc == ACT_DR || actTrunc == ACT_DIVE) {
        in->b = 0;
        in->x = (Utils::xoro_r(seed) % 256) - 128;
        in->y = (Utils::xoro_r(seed) % 256) - 128;
        return;
    }

    if (actTrunc == ACT_DR_LAND) {
        if (*marioHSpd > 0) {
            in->b = 0;
            in->x = 0;
            in->y = 0;
            return;
        }
        in->b = 0;
        in->x = (Utils::xoro_r(seed) % 256) - 128;
        in->y = (Utils::xoro_r(seed) % 256) - 128;
        return;
    }


    if (frame == 0 || Utils::xoro_r(seed) % jFact == 0) {
        int choice = Utils::xoro_r(seed) % 3;
        if (choice == 0) {  //fifd: with probability 1/3, random joystick
            in->x = (Utils::xoro_r(seed) % 256) - 128;
            in->y = (Utils::xoro_r(seed) % 256) - 128;
        }
        else if (choice == 1) { //fifd: with probability 1/3, go as close as we can to barely downhill
            int downhillAngle = 0;
            if (pyraZNorm != 0) {
                downhillAngle = ((int)(atan(*pyraXNorm / *pyraZNorm) * 32768.0 / M_PI)) % 65536;
            }
            int lefthillAngle = downhillAngle - 16384;
            int righthillAngle = downhillAngle + 16384;
            int lefthillDiff = *marioYawFacing - lefthillAngle;
            int tarAng = (int)*marioYawFacing - (int)*camYaw;
            if (abs(lefthillDiff + 65536 * 2) % 65536 < 16384 || abs(lefthillDiff + 65536 * 2) % 65536 > 49152) {
                tarAng = (int)lefthillAngle - (int)*camYaw + (Utils::xoro_r(seed) % 80 - 70);
            }
            else {
                tarAng = (int)righthillAngle - (int)*camYaw + (Utils::xoro_r(seed) % 80 - 10);
            }
            float tarRad = tarAng * M_PI / 32768.0;
            float tarX = 100 * sin(tarRad);
            float tarY = -100 * cos(tarRad);
            if (tarX > 0) { tarX += 6; }
            else { tarX -= 6; };
            if (tarY > 0) { tarY += 6; }
            else { tarY -= 6; };
            in->x = round(tarX);
            in->y = round(tarY);
        }
        else if (choice == 2) { //match yaw
            int tarAng = (int)*marioYawFacing - (int)*camYaw;
            float tarRad = tarAng * M_PI / 32768.0;
            float tarX = 100 * sin(tarRad);
            float tarY = -100 * cos(tarRad);
            if (tarX > 0) { tarX += 6; }
            else { tarX -= 6; };
            if (tarY > 0) { tarY += 6; }
            else { tarY -= 6; };
            in->x = round(tarX);
            in->y = round(tarY);
        }
    }


    int downhillAngle = 0;
    if (pyraZNorm != 0) {
        downhillAngle = ((int)(atan(*pyraXNorm / *pyraZNorm) * 32768.0 / M_PI)) % 65536;
    }
    int uphillDiff = (*marioYawFacing - downhillAngle + 32768 + 65536 * 2) % 65536;
    //check for pbdr conditions
    if (fabs(*pyraXNorm) + fabs(*pyraZNorm) > .6 && *marioHSpd >= 29.0 &&
        (uphillDiff < 6000 || uphillDiff > 59536) && Utils::xoro_r(seed) % 5 < 4 &&
        actTrunc == ACT_WALK) {
        //printf("did this\n");
        //printf("%f %f %f %d \n", pyraXNorm, pyraZNorm, marioHSpd, uphillDiff);
        int tarAng = (int)*marioYawFacing - (int)*camYaw + Utils::xoro_r(seed) % 14000 - 7000;
        float tarRad = tarAng * M_PI / 32768.0;
        float tarX = 100 * sin(tarRad);
        float tarY = -100 * cos(tarRad);
        if (tarX > 0) { tarX += 6; }
        else { tarX -= 6; };
        if (tarY > 0) { tarY += 6; }
        else { tarY -= 6; };
        in->x = round(tarX);
        in->y = round(tarY);
        in->b = 0;
        in->b |= CONT_B;  //dive
        in->b |= CONT_START;  //pause for pause buffer
        in->b |= CONT_DDOWN; //mark that we are on the first frame
        return;
    }
    if (actTrunc == ACT_DIVE_LAND) {  //dive landing after pause buffer
        //printf("did this 2\n");
        in->b = 0;
        in->b |= CONT_B; //dive recover
        in->x = (Utils::xoro_r(seed) % 256) - 128;
        in->y = (Utils::xoro_r(seed) % 256) - 128;
        return;
    }



    //reset the buttons and then enable the ones we chose to press
    in->b = 0;
    //if (doA) in->b |= CONT_A;  //No A presses allowed
    //if (doB) in->b |= CONT_B;
    //if (doZ) in->b |= CONT_G;
    //if (doC) in->b |= CONT_E;
}

//fifd: This function maps game states to a "truncated" version -
//that is, identifies the part of the state space partition this game state belongs to.
//output has 3 spatial coordinates (which cube in space Mario is in) and a variable called
//s, which contains information about the action, button presses, camera mode,
//hspd, and yaw
Vec3d truncFunc(HMODULE& dll)
{
    void* gMarioStates = GetProcAddress(dll, "gMarioStates");
    void* gObjectPool = GetProcAddress(dll, "gObjectPool");
    void* gCamera = GetProcAddress(dll, "gCamera");
    void* gControllers = (void*)GetProcAddress(dll, "gControllers");

    float* x = (float*)((char*)gMarioStates + 60);
    float* y = (float*)((char*)gMarioStates + 64);
    float* z = (float*)((char*)gMarioStates + 68);
    unsigned int* marioAction = (unsigned int*)((char*)gMarioStates + 12);
    uint16_t* marioYawFacing = (uint16_t*)((char*)gMarioStates + 46);
    float* marioHSpd = (float*)((char*)gMarioStates + 0x54);
    uint16_t* camYaw = (uint16_t*)((char*)gCamera + 340);

    unsigned short* controlButDown = (unsigned short*)((char*)gControllers + 0x10);

    float* pyraXNorm = (float*)((char*)gObjectPool + 84 * 1392 + 324);
    float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);
    float* pyraZNorm = (float*)((char*)gObjectPool + 84 * 1392 + 332);

    float* bullyX = (float*)((char*)gObjectPool + 57 * 1392 + 56);
    float* bullyY = (float*)((char*)gObjectPool + 57 * 1392 + 60);
    float* bullyZ = (float*)((char*)gObjectPool + 57 * 1392 + 64);

    float* marioYVel = (float*)((char*)gMarioStates + 76);

    uint64_t s = 0;
    unsigned int actTrunc = *marioAction & 0x1FF;
    if (actTrunc == ACT_BRAKE) s = 0;
    if (actTrunc == ACT_DIVE) s = 1;
    if (actTrunc == ACT_DIVE_LAND) s = 2;
    if (actTrunc == ACT_DR) s = 3;
    if (actTrunc == ACT_DR_LAND) s = 4;
    if (actTrunc == ACT_FREEFALL) s = 5;
    if (actTrunc == ACT_FREEFALL_LAND) s = 6;
    if (actTrunc == ACT_TURNAROUND_1) s = 7;
    if (actTrunc == ACT_TURNAROUND_2) s = 8;
    if (actTrunc == ACT_WALK) s = 9;

    s *= 30;
    s += (int)((40 - *marioYVel) / 4);

    float norm_regime_min = .69;
    //float norm_regime_max = .67;
    float target_xnorm = -.30725;
    float target_znorm = .3665;
    float x_delt = *pyraXNorm - target_xnorm;
    float z_delt = *pyraZNorm - target_znorm;
    float x_remainder = x_delt * 100 - floor(x_delt * 100);
    float z_remainder = z_delt * 100 - floor(z_delt * 100);


    //if((fabs(pyraXNorm) + fabs(pyraZNorm) < norm_regime_min) ||
    //   (fabs(pyraXNorm) + fabs(pyraZNorm) > norm_regime_max)){  //coarsen for bad norm regime
    if ((x_remainder > .001 && x_remainder < .999) || (z_remainder > .001 && z_remainder < .999) ||
        (fabs(*pyraXNorm) + fabs(*pyraZNorm) < norm_regime_min)) { //coarsen for not target norm envelope
        s *= 14;
        s += (int)((*pyraXNorm + 1) * 7);

        s *= 14;
        s += (int)((*pyraZNorm + 1) * 7);

        s += 1000000 + 1000000 * (int)floor((*marioHSpd + 20) / 8);
        s += 100000000 * (int)floor((float)*marioYawFacing / 16384.0);

        s *= 2;
        s += 1; //mark bad norm regime

        return Vec3d { (uint8_t)floor((*x + 2330) / 200), (uint8_t)floor((*y + 3200) / 400), (uint8_t)floor((*z + 1090) / 200), s };
    }
    s *= 200;
    s += (int)((*pyraXNorm + 1) * 100);

    float xzSum = fabs(*pyraXNorm) + fabs(*pyraZNorm);

    s *= 10;
    xzSum += (int)((xzSum - norm_regime_min) * 100);
    //s += (int)((pyraZNorm + 1)*100);

    s *= 30;
    s += (int)((*pyraYNorm - .7) * 100);

    //fifd: Hspd mapped into sections {0-1, 1-2, ...}
    s += 30000000 + 30000000 * (int)floor((*marioHSpd + 20));

    //fifd: Yaw mapped into sections
    //s += 100000000 * (int)floor((float)marioYawFacing / 2048.0);
    s += ((uint64_t)1200000000) * (int)floor((float)*marioYawFacing / 4096.0);

    s *= 2; //mark good norm regime

    return Vec3d { (uint8_t)floor((*x + 2330) / 10), (uint8_t)floor((*y + 3200) / 50), (uint8_t)floor((*z + 1090) / 10), s };
}



//UPDATED FOR SEGMENT STRUCT
int blockLength(Block a) {
    int len = 0;
    Segment* curSeg = a.tailSeg;
    if (a.tailSeg->depth == 0) { printf("tailSeg depth is 0!\n"); }
    while (curSeg != 0) {
        if (curSeg->depth == 0) { printf("curSeg depth is 0!\n"); }
        len += curSeg->numFrames;
        curSeg = curSeg->parent;
    }
    return len;
}

float dist3d(float a, float b, float c, float x, float y, float z) {
    return sqrt((a - x) * (a - x) + (b - y) * (b - y) + (c - z) * (c - z));
}

int angDist(unsigned short a, unsigned short b) {
    int dist = abs((int)a - (int)b);
    if (dist > 32768) dist = 65536 - dist;
    return dist;
}

int vibeCheck(float tarX, float tarZ, float tarT, float spd, float curX, float curZ, float curT) {
    float dist = sqrt((curX - tarX) * (curX - tarX) + (curZ - tarZ) * (curZ - tarZ));
    if (dist < (tarT - curT)* spd) return 1;
    return 0;
}

int leftLine(float a, float b, float c, float d, float e, float f) {
    return (b - d) * (e - a) + (c - a) * (f - b) > 0 ? 0 : 1;
}











void InitConfiguration(Configuration& configuration)
{
    configuration.StartFrame = 3545;
    configuration.SegmentLength = 10;
    configuration.MaxSegments = 1024;
    configuration.MaxBlocks = 500000;
    configuration.MaxHashes = 10 * configuration.MaxBlocks;
    configuration.MaxSharedBlocks = 20000000;
    configuration.MaxSharedHashes = 10 * configuration.MaxSharedBlocks;
    configuration.TotalThreads = 4;
    configuration.MaxSharedSegments = 25000000;
    configuration.MaxLocalSegments = 2000000;
    configuration.MaxLightningLength = 10000;
}

void MergeBlocks(Configuration& config, GlobalState& gState, Printer& printer)
{
    printer.printfQ("Merging blocks.\n");

    int otid, n, m;
    for (otid = 0; otid < config.TotalThreads; otid++) {
        for (n = 0; n < gState.NBlocks[otid]; n++) {
            Block tmpBlock = gState.AllBlocks[otid * config.MaxBlocks + n];
            m = tmpBlock.pos.findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, 0, gState.NBlocks[config.TotalThreads]);
            if (m < gState.NBlocks[config.TotalThreads]) {
                if (tmpBlock.value > gState.SharedBlocks[m].value) { // changed to >
                    gState.SharedBlocks[m] = tmpBlock;
                }
            }
            else {
                gState.SharedHashTab[tmpBlock.pos.findNewHashInx(gState.SharedHashTab, config.MaxSharedHashes)] = gState.NBlocks[config.TotalThreads];
                gState.SharedBlocks[gState.NBlocks[config.TotalThreads]++] = tmpBlock;
            }
        }
    }

    memset(gState.AllHashTabs, 0xFF, config.MaxHashes * config.TotalThreads * sizeof(int)); // Clear all local hash tables.

    for (otid = 0; otid < config.TotalThreads; otid++) {
        gState.NBlocks[otid] = 0; // Clear all local blocks.
    }
}

void MergeSegments(Configuration& config, GlobalState& gState)
{
    printf("Merging segments\n");

    // Get reference counts for each segment. Tried to track this but ran into
    // multi-threading issues, so might as well recompute here.
    for (int threadNum = 0; threadNum < config.TotalThreads; threadNum++) {
        for (int segInd = threadNum * config.MaxLocalSegments; segInd < threadNum * config.MaxLocalSegments + gState.NSegments[threadNum]; segInd++) {
            //printf("%d %d\n", segInd, numSegs[totThreads]);
            gState.AllSegments[config.TotalThreads * config.MaxLocalSegments + gState.NSegments[config.TotalThreads]] = gState.AllSegments[segInd];
            gState.NSegments[config.TotalThreads]++;
            gState.AllSegments[segInd] = 0;
        }
        gState.NSegments[threadNum] = 0;
    }
}

void SegmentGarbageCollection(Configuration& config, GlobalState& gState)
{
    printf("Segment garbage collection. Start with %d segments\n", gState.NSegments[config.TotalThreads]);

    for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + gState.NSegments[config.TotalThreads]; segInd++) {
        gState.AllSegments[segInd]->refCount = 0;
    }
    for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + gState.NSegments[config.TotalThreads]; segInd++) {
        if (gState.AllSegments[segInd]->parent != 0) { gState.AllSegments[segInd]->parent->refCount++; }
    }
    for (int blockInd = 0; blockInd < gState.NBlocks[config.TotalThreads]; blockInd++) {
        gState.SharedBlocks[blockInd].tailSeg->refCount++;
    }
    for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + gState.NSegments[config.TotalThreads]; segInd++) {
        Segment* curSeg = gState.AllSegments[segInd];
        if (curSeg->refCount == 0) {
            //printf("removing a seg\n");
            if (curSeg->parent != 0) { curSeg->parent->refCount -= 1; }
            //printf("moving %d %d\n", segInd, totThreads*maxLocalSegs+numSegs[totThreads]);
            gState.AllSegments[segInd] = gState.AllSegments[config.TotalThreads * config.MaxLocalSegments + gState.NSegments[config.TotalThreads] - 1];
            gState.NSegments[config.TotalThreads]--;
            segInd--;
            free(curSeg);
        }
    }

    printf("Segment garbage collection finished. Ended with %d segments\n", gState.NSegments[config.TotalThreads]);
}

Input* GetM64(const char* path)
{
    Input in;
    int length = 83600;
    size_t inputSize = sizeof(Input);
    size_t fileSize = inputSize * length;
    Input* fileInputs = (Input*)malloc(fileSize);

    FILE* fp = fopen(path, "rb");
    fseek(fp, 0x400, SEEK_SET);
    
    for (int i = 0; i < length; i++) {
        fread(&in, inputSize, 1, fp);
        in.b = (in.b >> 8) | (in.b << 8); // Fuck me endianness
        fileInputs[i] = in;
    }

    fclose(fp);

    return fileInputs;
}





void AdvanceToStart(Configuration& config, ThreadState& tState, SaveState& saveState, Dll& dll, Input* fileInputs)
{
    Input* gControllerPads = (Input*)GetProcAddress(dll.hdll, "gControllerPads");
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_update");

    for (int f = 0; f < config.StartFrame + 5; f++) {
        *gControllerPads = tState.CurrentInput = fileInputs[f];
        sm64_update();
        if (f == config.StartFrame - 1) saveState.save(dll);
    }
}

void ProcessNewBlock(Configuration& config, GlobalState& gState, ThreadState& tState, uint64_t prevRngSeed, int nFrames, Vec3d newPos, float newFitness)
{
    Block newBlock;

    // Create and add block to list.
    if (gState.NBlocks[tState.Id] == config.MaxBlocks) {
        printf("Max local blocks reached!\n");
    }
    else {
        //UPDATED FOR SEGMENTS STRUCT
        newBlock = tState.BaseBlock;
        newBlock.pos = newPos;
        newBlock.value = newFitness;
        int blInxLocal = newPos.findBlock(tState.Blocks, tState.HashTab, config.MaxHashes, 0, gState.NBlocks[tState.Id]);
        int blInx = newPos.findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, 0, gState.NBlocks[config.TotalThreads]);

        if (blInxLocal < gState.NBlocks[tState.Id]) { // Existing local block.
            if (newBlock.value >= tState.Blocks[blInxLocal].value) {
                Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                newSeg->parent = tState.BaseBlock.tailSeg;
                newSeg->refCount = 0;
                newSeg->numFrames = nFrames + 1;
                newSeg->seed = prevRngSeed;
                newSeg->depth = tState.BaseBlock.tailSeg->depth + 1;
                if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                if (tState.BaseBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                newBlock.tailSeg = newSeg;
                gState.AllSegments[tState.Id * config.MaxLocalSegments + gState.NSegments[tState.Id]] = newSeg;
                gState.NSegments[tState.Id] += 1;
                tState.Blocks[blInxLocal] = newBlock;
            }
        }
        else if (blInx < gState.NBlocks[config.TotalThreads] && newBlock.value < gState.SharedBlocks[blInx].value);// Existing shared block but worse.
        else { // Existing shared block and better OR completely new block.
            tState.HashTab[newPos.findNewHashInx(tState.HashTab, config.MaxHashes)] = gState.NBlocks[tState.Id];
            Segment* newSeg = (Segment*)malloc(sizeof(Segment));
            newSeg->parent = tState.BaseBlock.tailSeg;
            newSeg->refCount = 1;
            newSeg->numFrames = nFrames + 1;
            newSeg->seed = prevRngSeed;
            newSeg->depth = tState.BaseBlock.tailSeg->depth + 1;
            if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
            if (tState.BaseBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
            newBlock.tailSeg = newSeg;
            gState.AllSegments[tState.Id * config.MaxLocalSegments + gState.NSegments[tState.Id]] = newSeg;
            gState.NSegments[tState.Id] += 1;
            tState.Blocks[gState.NBlocks[tState.Id]++] = newBlock;
        }
    }
}

void PrintStatus(Configuration& config, GlobalState& gState, ThreadState& tState, int mainIteration, Printer& printer)
{
    printer.printfQ("\nThread ALL Loop %d blocks %d\n", mainIteration, gState.NBlocks[config.TotalThreads]);
    printer.printfQ("LOAD %.3f RUN %.3f BLOCK %.3f TOTAL %.3f\n", tState.LoadTime, tState.RunTime, tState.BlockTime, omp_get_wtime() - tState.LoopTimeStamp);
    printer.printfQ("\n\n");

    tState.LoadTime = tState.RunTime = tState.BlockTime = 0;
    tState.LoopTimeStamp = omp_get_wtime();
}


int DecodeAndExecuteDiff(Configuration& config, ThreadState& tState, Input* m64Diff, HMODULE& dll)
{
    Input* gControllerPads = (Input*)GetProcAddress(dll, "gControllerPads");
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll, "sm64_update");

    int frameOffset = 0;

    //UPDATED FOR SEGMENTS STRUCT
    //Before, I temporarily reversed the linked list.
    //But this doesn't work in a multi threaded environment.
    //Punt and do this in quadratic time. This shouldn't be a
    //bottleneck anyway but can fix it if needed
    if (tState.BaseBlock.tailSeg == 0)
        printf("origBlock has null tailSeg");

    Segment* thisTailSeg = tState.BaseBlock.tailSeg;
    Segment* curSeg;
    int thisSegDepth = thisTailSeg->depth;
    for (int i = 1; i <= thisSegDepth; i++) {
        curSeg = thisTailSeg;
        while (curSeg->depth != i) {  //inefficient but probably doesn't matter
            if (curSeg->parent == 0)
                printf("Parent is null!");
            if (curSeg->parent->depth + 1 != curSeg->depth) { printf("Depths wrong"); }
            curSeg = curSeg->parent;
        }

        //Run the inputs
        uint64_t tmpSeed = curSeg->seed;
        int megaRandom = Utils::xoro_r(&tmpSeed) % 2;
        for (int f = 0; f < curSeg->numFrames; f++) {
            perturbInput(dll, &tState.CurrentInput, &tmpSeed, frameOffset, megaRandom);
            m64Diff[frameOffset++] = tState.CurrentInput;
            *gControllerPads = tState.CurrentInput;
            sm64_update();

            tState.UpdateLightning(config, truncFunc(dll));
        }
    }

    return frameOffset;
}

bool ValidateBlock(Configuration& config, ThreadState& tState, HMODULE& dll, Input* m64Diff, int frame)
{
    void* gMarioStates = GetProcAddress(dll, "gMarioStates");
    void* gObjectPool = GetProcAddress(dll, "gObjectPool");
    void* gCamera = GetProcAddress(dll, "gCamera");

    float* marioX = (float*)((char*)gMarioStates + 60);
    float* marioY = (float*)((char*)gMarioStates + 64);
    float* marioZ = (float*)((char*)gMarioStates + 68);
    unsigned int* marioAction = (unsigned int*)((char*)gMarioStates + 12);
    uint16_t* marioYawFacing = (uint16_t*)((char*)gMarioStates + 46);
    float* marioHSpd = (float*)((char*)gMarioStates + 0x54);
    uint16_t* camYaw = (uint16_t*)((char*)gCamera + 340);
    float* marioYVel = (float*)((char*)gMarioStates + 76);
    float* marioFloorHeight = (float*)((char*)gMarioStates + 0x07C);

    float* pyraXNorm = (float*)((char*)gObjectPool + 84 * 1392 + 324);
    float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);
    float* pyraZNorm = (float*)((char*)gObjectPool + 84 * 1392 + 332);

    unsigned int actionTrunc = *marioAction & 0x1FF;

    if (*marioX < -2330) return false;
    if (*marioX > -1550) return false;
    if (*marioZ < -1090) return false;
    if (*marioZ > -300) return false;
    if (*marioY > -2760) return false;
    if (*pyraZNorm < -.15 || *pyraXNorm > 0.15) return false; //stay in desired quadrant
    if (actionTrunc != ACT_BRAKE && actionTrunc != ACT_DIVE && actionTrunc != ACT_DIVE_LAND &&
        actionTrunc != ACT_DR && actionTrunc != ACT_DR_LAND && actionTrunc != ACT_FREEFALL &&
        actionTrunc != ACT_FREEFALL_LAND && actionTrunc != ACT_TURNAROUND_1 &&
        actionTrunc != ACT_TURNAROUND_2 && actionTrunc != ACT_WALK) {
        return false;
    } //not useful action, such as lava boost
    if (actionTrunc == ACT_FREEFALL && *marioYVel > -20.0) return false;//freefall without having done nut spot chain
    if (*marioFloorHeight > -3071 && *marioY > *marioFloorHeight + 4 &&
        *marioYVel != 22.0) return false;//above pyra by over 4 units
    if (*marioFloorHeight == -3071 && actionTrunc != ACT_FREEFALL) return false; //diving/dring above lava

    if (actionTrunc == ACT_DR && fabs(*pyraXNorm) > .3 && fabs(*pyraXNorm) + fabs(*pyraZNorm) > .65 &&
        *marioX + *marioZ > (-1945 - 715)) {  //make sure Mario is going toward the right/east edge
        char fileName[128];
        //printf("dr\n");
        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\m64s\\dr\\bitfs_dr_%f_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel, tState.Id);
        Utils::writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64", m64Diff, config.StartFrame, frame + 1);
    }

    //check on hspd > 1 confirms we're in dr land rather than quickstopping,
    //which gives the same action
    if (actionTrunc == ACT_DR_LAND && *marioY > -2980 && *marioHSpd > 1
        && fabs(*pyraXNorm) > .29 && fabs(*marioX) > -1680) {
        char fileName[128];
        //if(printingDRLand > 0)printf("dr land\n");
        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\m64s\\drland\\bitfs_drland_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, tState.Id);
        Utils::writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64", m64Diff, config.StartFrame, frame + 1);
    }

    return true;
}

float StateBinFitness(HMODULE& dll)
{
    void* gObjectPool = GetProcAddress(dll, "gObjectPool");
    float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);

    return *pyraYNorm;
}

void ExtendTasFromBlock(Configuration& config, GlobalState& gState, ThreadState& tState, HMODULE& dll,
    Input* m64Diff, int frameOffset, int megaRandom, uint64_t baseRngSeed, Vec3d prevStateBin)
{
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll, "sm64_update");
    Input* gControllerPads = (Input*)GetProcAddress(dll, "gControllerPads");

    for (int f = 0; f < config.SegmentLength; f++) {
        perturbInput(dll, &tState.CurrentInput, &tState.RngSeed, frameOffset + f, megaRandom);
        m64Diff[frameOffset + f] = tState.CurrentInput;
        *gControllerPads = tState.CurrentInput;

        auto timerStart = omp_get_wtime();
        sm64_update();
        tState.RunTime += omp_get_wtime() - timerStart;

        if (!tState.ValidateCourseAndArea(dll) || !ValidateBlock(config, tState, dll, m64Diff, frameOffset + f))
            break;

        Vec3d newStateBin = truncFunc(dll);
        tState.UpdateLightning(config, newStateBin);

        //fifd: Checks to see if we're in a new Block. If so, save off the segment so far.
        timerStart = omp_get_wtime();
        if (!newStateBin.truncEq(prevStateBin) && !newStateBin.truncEq(tState.BaseBlock.pos))
        {
            // Create and add block to list.
            ProcessNewBlock(config, gState, tState, baseRngSeed, f, newStateBin, StateBinFitness(dll));

            prevStateBin = newStateBin; // TODO: Why this here?
        }
        tState.BlockTime += omp_get_wtime() - timerStart;
    }
}





void MergeState(Configuration& config, GlobalState& gState, ThreadState& tState, int mainIteration, Printer& printer)
{
    // Merge all blocks from all threads and redistribute info.
    MergeBlocks(config, gState, printer);

    // Handle segments
    MergeSegments(config, gState);

    if (mainIteration % 3000 == 0)
        SegmentGarbageCollection(config, gState);

    PrintStatus(config, gState, tState, mainIteration, printer);

    printer.flushLog();
}

void main(int argc, char* argv[])
{
    Printer printer;
    printer.ParseArgs(argc, argv);

    Configuration config;
    InitConfiguration(config);
    GlobalState gState = GlobalState(config);

    omp_set_num_threads(config.TotalThreads);
    Utils::MultiThread([&]()
        {
            //TODO: Maybe don't hardcode DLLs
            LPCWSTR dlls[4] = { L"sm64_jp_0.dll", L"sm64_jp_1.dll" , L"sm64_jp_2.dll" , L"sm64_jp_3.dll" };
            ThreadState tState = ThreadState(config, gState, omp_get_thread_num());
            Dll dll;
            dll.getDllInfo(dlls[tState.Id]);
            SaveState state, state2;
            state.allocState(dll);
            state2.allocState(dll);
            Input* m64Diff = (Input*)malloc(sizeof(Input) * (config.SegmentLength * config.MaxSegments + 256)); // Todo: Nasty

            // Initialize game
            VOIDFUNC sm64_init = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_init");
            sm64_init();

            // Read inputs from file and advance to start frame
            Input* fileInputs = GetM64("C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64");
            AdvanceToStart(config, tState, state, dll, fileInputs);
            tState.LoadTime += state.riskyLoadJ(dll);

            // Initialize thread state
            tState.Initialize(config, gState, truncFunc(dll.hdll), dll.hdll);

            for (int mainIteration = 0; mainIteration <= 1000000000; mainIteration++) {
                // ALWAYS START WITH A MERGE SO THE SHARED BLOCKS ARE OK.
                if (mainIteration % 300 == 0)
                    Utils::SingleThread([&](){ MergeState(config, gState, tState, mainIteration, printer); });

                // Pick a block to "fire a scattershot" at
                if (!tState.SelectBaseBlock(config, gState, mainIteration))
                    break;

                // Revert to initial state, and advance game state to end of block diff
                tState.LoadTime += state.riskyLoadJ(dll);
                tState.LightningLengthLocal = 0;
                tState.LightningLocal[tState.LightningLengthLocal++] = truncFunc(dll.hdll);
                int frameOffset = DecodeAndExecuteDiff(config, tState, m64Diff, dll.hdll);
                state2.save(dll);

                // Sanity check that state matches saved block state
                if (!tState.ValidateBaseBlock(truncFunc(dll.hdll)))
                    return;

                // "Fire" the scattershot, i.e. execute a batch of semi-random inputs from the base block state.
                Input origLastIn = tState.CurrentInput;
                int origLightLenLocal = tState.LightningLengthLocal;
                for (int subLoop = 0; subLoop < 200; subLoop++) {
                    tState.LoadTime += state2.riskyLoadJ(dll);

                    tState.CurrentInput = origLastIn;
                    tState.LightningLengthLocal = origLightLenLocal;

                    uint64_t baseRngSeed = tState.RngSeed;
                    int megaRandom = Utils::xoro_r(&tState.RngSeed) % 2;
                    ExtendTasFromBlock(config, gState, tState, dll.hdll, m64Diff, frameOffset, megaRandom, baseRngSeed, tState.BaseBlock.pos);
                }
            }
        });
}
