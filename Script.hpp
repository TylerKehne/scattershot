#pragma once
#include "Utils.hpp"

#ifndef SCRIPT_H
#define SCRIPT_H

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

class Script
{
public:
    Configuration& config;
    GlobalState& gState;
    ThreadState& tState;
    Dll& dll;

    int StartCourse;
    int StartArea;

    Script(Configuration& config, GlobalState& gState, ThreadState& tState, Dll& dll) : config(config), gState(gState), tState(tState), dll(dll)
    {


    }

    void Initialize(Vec3d initTruncPos)
    {
        tState.Initialize(config, gState, initTruncPos);

        // Record start course/area for validation (generally scattershot has no cross-level value)
        StartCourse = *(short*)GetProcAddress(dll.hdll, "gCurrCourseNum");
        StartArea = *(short*)GetProcAddress(dll.hdll, "gCurrAreaIndex");
    }

    bool ValidateCourseAndArea()
    {
        return StartCourse == *(short*)GetProcAddress(dll.hdll, "gCurrCourseNum")
            && StartArea == *(short*)GetProcAddress(dll.hdll, "gCurrAreaIndex");
    }

    int DecodeAndExecuteDiff(Input* m64Diff)
    {
        Input* gControllerPads = (Input*)GetProcAddress(dll.hdll, "gControllerPads");
        VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_update");

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
                perturbInput(&tState.CurrentInput, &tmpSeed, frameOffset, megaRandom);
                m64Diff[frameOffset++] = tState.CurrentInput;
                *gControllerPads = tState.CurrentInput;
                sm64_update();

                tState.UpdateLightning(config, GetStateBin());
            }
        }

        return frameOffset;
    }

    void ExtendTasFromBlock(Input* m64Diff, int frameOffset, int megaRandom, uint64_t baseRngSeed, Vec3d prevStateBin)
    {
        VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_update");
        Input* gControllerPads = (Input*)GetProcAddress(dll.hdll, "gControllerPads");

        for (int f = 0; f < config.SegmentLength; f++) {
            perturbInput(&tState.CurrentInput, &tState.RngSeed, frameOffset + f, megaRandom);
            m64Diff[frameOffset + f] = tState.CurrentInput;
            *gControllerPads = tState.CurrentInput;

            auto timerStart = omp_get_wtime();
            sm64_update();
            tState.RunTime += omp_get_wtime() - timerStart;

            if (!ValidateCourseAndArea() || !ValidateBlock(m64Diff, frameOffset + f))
                break;

            Vec3d newStateBin = GetStateBin();
            tState.UpdateLightning(config, newStateBin);

            //fifd: Checks to see if we're in a new Block. If so, save off the segment so far.
            timerStart = omp_get_wtime();
            if (!newStateBin.truncEq(prevStateBin) && !newStateBin.truncEq(tState.BaseBlock.pos))
            {
                // Create and add block to list.
                tState.ProcessNewBlock(config, gState, baseRngSeed, f, newStateBin, StateBinFitness());

                prevStateBin = newStateBin; // TODO: Why this here?
            }
            tState.BlockTime += omp_get_wtime() - timerStart;
        }
    }

    //fifd: Where new inputs to try are actually produced
    //I think this is a perturbation of the previous frame's input to be used for the upcoming frame
    void perturbInput(Input* in, uint64_t* seed, int frame, int megaRandom) {
        void* gMarioStates = GetProcAddress(dll.hdll, "gMarioStates");
        void* gObjectPool = GetProcAddress(dll.hdll, "gObjectPool");
        void* gCamera = GetProcAddress(dll.hdll, "gCamera");

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
    Vec3d GetStateBin()
    {
        void* gMarioStates = GetProcAddress(dll.hdll, "gMarioStates");
        void* gObjectPool = GetProcAddress(dll.hdll, "gObjectPool");
        void* gCamera = GetProcAddress(dll.hdll, "gCamera");
        void* gControllers = (void*)GetProcAddress(dll.hdll, "gControllers");

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

            return Vec3d{ (uint8_t)floor((*x + 2330) / 200), (uint8_t)floor((*y + 3200) / 400), (uint8_t)floor((*z + 1090) / 200), s };
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

        return Vec3d{ (uint8_t)floor((*x + 2330) / 10), (uint8_t)floor((*y + 3200) / 50), (uint8_t)floor((*z + 1090) / 10), s };
    }

    float StateBinFitness()
    {
        void* gObjectPool = GetProcAddress(dll.hdll, "gObjectPool");
        float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);

        return *pyraYNorm;
    }

    bool ValidateBlock(Input* m64Diff, int frame)
    {
        void* gMarioStates = GetProcAddress(dll.hdll, "gMarioStates");
        void* gObjectPool = GetProcAddress(dll.hdll, "gObjectPool");
        void* gCamera = GetProcAddress(dll.hdll, "gCamera");

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

    void AdvanceToStart(SaveState& saveState, Input* fileInputs)
    {
        Input* gControllerPads = (Input*)GetProcAddress(dll.hdll, "gControllerPads");
        VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_update");

        for (int f = 0; f < config.StartFrame + 5; f++) {
            *gControllerPads = tState.CurrentInput = fileInputs[f];
            sm64_update();
            if (f == config.StartFrame - 1) saveState.save(dll);
        }
    }
};

#endif