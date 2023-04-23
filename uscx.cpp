#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <windows.h>
#include <time.h>
#include <dbghelp.h>
#include <omp.h>
#include <winbase.h>
#include <stdarg.h>
#include <stdint.h>

#define _USE_MATH_DEFINES
#include <math.h>

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



char dataMap[8192] = "  0 ...........................X...........XX.XXX.X....................................................."
"  1 ...................................................................................................."
"  2 ...................................................................................................."
"  3 ...................................................................................................."
"  4 ...................................................................................................."
"  5 ...................................................................................................."
"  6 ...................................................................................................."
"  7 ...................................................................................................."
"  8 ...................................................................................................."
"  9 ...................................................................................................."
" 10 ...................................................................................................."
" 11 ...................................................................................................."
" 12 ...................................................................................................."
" 13 ...................................................................................................."
" 14 ...................................................................................................."
" 15 ...................................................................................................."
" 16 ...................................................................................................."
" 17 ...................................................................................................."
" 18 ...................................................................................................."
" 19 ...................................................................................................."
" 20 ...................................................................................................."
" 21 ...................................................................................................."
" 22 ...................................................................................................."
" 23 ...................................................................................................."
" 24 ...................................................................................................."
" 25 ...........................X........................................................................";

char bssMap[8192] = "  0 XX.....XXX.X................XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  1 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  2 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  3 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.XXXXXX................XXXXX.."
"  4 ...............................X.......XXXXXXXXXXXX................................................."
"  5 ..................XXXXXXXXXXXXXXXXXXXXXXXX.........................................................."
"  6 ...................................................................................................."
"  7 ...................................................................................................."
"  8 ...................................................................................................."
"  9 ...................................................................................................."
" 10 ...................................................................................................."
" 11 ...................................................................................................."
" 12 ...................................................................................................."
" 13 ...................................................................................................."
" 14 ...................................................................................................."
" 15 ...................................................................................................."
" 16 ...................................................................................................."
" 17 .....................................................................XX............................."
" 18 ..................XXXX....X.......XXXXXXXX...............XXXX...........X.XXX..........XXXX........."
" 19 ................XXX..X.X...X.X..XX.X..........X...X.........XX......................................"
" 20 ........................XX.........................................................................."
" 21 ...........................XX......................................................................."
" 22 ......XXXXXX........................................................................................";


typedef void (CALLBACK* VOIDFUNC)();
typedef void* (CALLBACK* GFXFUNC)(int, void*, void*, void*);

typedef struct {
    unsigned short b;
    char x;
    char y;
} Input;

typedef struct {
    void* data;
    void* bss;
} SaveState;

typedef struct Segment Segment;

struct Segment {
    Segment* parent;
    uint64_t seed;
    uint32_t refCount;
    uint8_t numFrames;
    uint8_t depth;
};

//fifd: Vec3d actually has 4 dimensions, where the first 3 are spatial and
//the 4th encodes a lot of information about Mario's state (actions, speed,
//camera) as well as some button information. These vectors specify a part
//of state space.
typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint64_t s;
} Vec3d;

//fifd: I think this is an element of the partition of state
//space. Will need to understand what each of its fields are
typedef struct {
    Vec3d         pos;    //fifd: an output of truncFunc. Identifies which block this is
    float         value;    //fifd: a fitness of the best TAS that reaches this block; the higher the better.
    Segment* tailSeg;
    //Time is most important component of value but keeps higher hspeed if time is tied
} Block;

typedef struct {
    float x, y, z;
    int actTrunc;
    unsigned short yawFacing;
    float hspd;
} FinePos;

int dataStart, dataLength, bssStart, bssLength;
int gPrint = 1, gLog = 1;
char gProgName[192] = { 0 };
FILE* gLogFP = NULL;

void printfQ(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (gPrint) vprintf(format, args);
    if (gLog) {
        if (!gLogFP) {
            char logName[256] = { 0 };
            sprintf(logName, "%s_log.txt", gProgName);
            gLogFP = fopen(logName, "a");
        }
        vfprintf(gLogFP, format, args);
    }
    va_end(args);
}

void flushLog() {
    fclose(gLogFP);
    gLogFP = NULL;
}

//fifd: only called by xoro_r
static inline uint32_t rotl(const uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

//fifd: random number generation utility
uint32_t xoro_r(uint64_t* s) {
    const uint32_t s0 = *s >> 32;
    uint32_t s1 = (uint32_t)(*s) ^ s0;
    *s = (uint64_t)(rotl(s0, 26) ^ s1 ^ (s1 << 9)) << 32 | rotl(s1, 13);
    return rotl(s0 * 0x9E3779BB, 5) * 5;
}

void getDllInfo(HMODULE hDLL) {
    IMAGE_NT_HEADERS* pNtHdr = ImageNtHeader(hDLL);
    IMAGE_SECTION_HEADER* pSectionHdr = (IMAGE_SECTION_HEADER*)(pNtHdr + 1);

    for (int i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++) {
        char* name = (char*)pSectionHdr->Name;
        if (strcmp(name, ".data") == 0) {
            dataStart = pSectionHdr->VirtualAddress;
            dataLength = pSectionHdr->Misc.VirtualSize;
        }
        if (strcmp(name, ".bss") == 0) {
            bssStart = pSectionHdr->VirtualAddress;
            bssLength = pSectionHdr->Misc.VirtualSize;
        }
        pSectionHdr++;
    }
}

void allocState(SaveState* s) {
    s->data = calloc(dataLength, 1);
    s->bss = calloc(bssLength, 1);
}

void allocStateSmall(SaveState* s) {
    s->data = calloc(10000, 1);
    s->bss = calloc(bssLength, 1);
}

void freeState(SaveState* s) {
    free(s->data);
    free(s->bss);
    s->data = s->bss = NULL;
}

void load(HMODULE hDLL, SaveState* s) {
    memcpy((char*)hDLL + dataStart, (char*)s->data, dataLength);
    memcpy((char*)hDLL + bssStart, (char*)s->bss, bssLength);
}

void riskyLoad(HMODULE hDLL, SaveState* s) {
    memcpy((char*)hDLL + dataStart + 0, (char*)s->data + 0, 100000);
    memcpy((char*)hDLL + dataStart + 25 * 100000, (char*)s->data + 25 * 100000, 100000);
    memcpy((char*)hDLL + bssStart + 0, (char*)s->bss + 0, 6 * 100000);
    memcpy((char*)hDLL + bssStart + 17 * 100000, (char*)s->bss + 17 * 100000, 6 * 100000);
    memcpy((char*)hDLL + bssStart + 47 * 100000, (char*)s->bss + 47 * 100000, 100000);
}

void riskyLoadJ(HMODULE hDLL, SaveState* s) {
    memcpy((char*)hDLL + dataStart + 0, (char*)s->data + 0, 100000);
    memcpy((char*)hDLL + dataStart + 20 * 100000, (char*)s->data + 20 * 100000, 100000);
    memcpy((char*)hDLL + bssStart + 0, (char*)s->bss + 0, 6 * 100000);
    memcpy((char*)hDLL + bssStart + 17 * 100000, (char*)s->bss + 17 * 100000, 6 * 100000);
    memcpy((char*)hDLL + bssStart + 47 * 100000, (char*)s->bss + 47 * 100000, 100000);
}

void riskyLoad2(HMODULE hDLL, SaveState* s) {
    int off = 0;
    for (int i = 0; i < strlen(dataMap); i++) {
        if (dataMap[i] == 'X') {
            memcpy((char*)hDLL + dataStart + off, (char*)s->data + off, 1000);
            off += 1000;
        }
        if (dataMap[i] == '.') off += 1000;
    }
    off = 0;
    for (int i = 0; i < strlen(bssMap); i++) {
        if (bssMap[i] == 'X') {
            if (!(off < 400000 && off >= 28000)) {
                memcpy((char*)hDLL + bssStart + off, (char*)s->bss + off, 1000);
            }
            off += 1000;
        }
        if (bssMap[i] == '.') off += 1000;
    }
    memcpy((char*)hDLL + bssStart + 28000, (char*)s->bss + 28000, 372000);
    memcpy((char*)hDLL + bssStart + 4742000, (char*)s->bss + 4742000, 1000);
}

void save(HMODULE hDLL, SaveState* s) {
    memcpy((char*)s->data, (char*)hDLL + dataStart, dataLength);
    memcpy((char*)s->bss, (char*)hDLL + bssStart, bssLength);
}

//fifd:  All references to this function are commented out
void xorStates(const SaveState* s1, const SaveState* s2, SaveState* s3, int tid) {
    char* a = (char*)s1->data;
    char* b = (char*)s2->data;
    char* c = (char*)s3->data;
    for (int i = 0; i < dataLength; i++) {
        c[i] |= b[i] ^ a[i];
    }
    a = (char*)s1->bss;
    b = (char*)s2->bss;
    c = (char*)s3->bss;
    int found = 0;
    for (int i = 0; i < bssLength; i++) {
        if (!found && !c[i] && (b[i] ^ a[i])) {
            printf("New at off %d!\n", i / 1000);
            found = 1;
        }
        if (i % 1000 == 0) found = 0;
        c[i] |= b[i] ^ a[i];
    }
}

//fifd:  All references to this function are commented out
void orStates(SaveState* s1, SaveState* s2) {
    char* a = (char*)s1->data;
    char* b = (char*)s2->data;
    for (int i = 0; i < dataLength; i++) {
        a[i] = b[i] = b[i] | a[i];
    }
    a = (char*)s1->bss;
    b = (char*)s2->bss;
    for (int i = 0; i < bssLength; i++) {
        a[i] = b[i] = b[i] | a[i];
    }
}

void writeFile(char* newFile, const char* base, Input* inputs, int offset, int length) {
    FILE* fp0 = fopen(newFile, "rb");
    if (fp0 != NULL)
    {
        fclose(fp0);
        return;
    }

    FILE* fp1 = fopen(base, "rb");
    FILE* fp2 = fopen(newFile, "wb");
    Input in;
    int i;

    for (i = 0; i < 0x400 + offset * 4; i++) {
        unsigned char a;
        fread(&a, 1, 1, fp1);
        fwrite(&a, 1, 1, fp2);
    }

    for (i = 0; i < length; i++) {
        in = inputs[i];
        in.b = (in.b >> 8) | (in.b << 8); // Fuck me endianness
        fwrite(&in, sizeof(Input), 1, fp2);
    }

    fclose(fp1);
    fclose(fp2);
}

void copyDll(char* newFile, char* base) {
    FILE* fp1 = fopen(base, "rb");
    FILE* fp2 = fopen(newFile, "wb");
    unsigned char a;
    while (fread(&a, 1, 1, fp1)) fwrite(&a, 1, 1, fp2);
    fclose(fp1);
    fclose(fp2);
}

//fifd: Where new inputs to try are actually produced
//I think this is a perturbation of the previous frame's input to be used for the upcoming frame
void perturbInput(Input* in, uint64_t* seed, int frame, unsigned int marioAction,
    uint16_t marioYawFacing, float pyraXNorm, float pyraZNorm, float marioHSpd,
    uint16_t camYaw, int megaRandom) {
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

    unsigned int actTrunc = marioAction & 0x1FF;

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
        in->x = (xoro_r(seed) % 256) - 128;
        in->y = (xoro_r(seed) % 256) - 128;
        return;
    }

    if (actTrunc == ACT_DR_LAND) {
        if (marioHSpd > 0) {
            in->b = 0;
            in->x = 0;
            in->y = 0;
            return;
        }
        in->b = 0;
        in->x = (xoro_r(seed) % 256) - 128;
        in->y = (xoro_r(seed) % 256) - 128;
        return;
    }


    if (frame == 0 || xoro_r(seed) % jFact == 0) {
        int choice = xoro_r(seed) % 3;
        if (choice == 0) {  //fifd: with probability 1/3, random joystick
            in->x = (xoro_r(seed) % 256) - 128;
            in->y = (xoro_r(seed) % 256) - 128;
        }
        else if (choice == 1) { //fifd: with probability 1/3, go as close as we can to barely downhill
            int downhillAngle = 0;
            if (pyraZNorm != 0) {
                downhillAngle = ((int)(atan(pyraXNorm / pyraZNorm) * 32768.0 / M_PI)) % 65536;
            }
            int lefthillAngle = downhillAngle - 16384;
            int righthillAngle = downhillAngle + 16384;
            int lefthillDiff = marioYawFacing - lefthillAngle;
            int tarAng = (int)marioYawFacing - (int)camYaw;
            if (abs(lefthillDiff + 65536 * 2) % 65536 < 16384 || abs(lefthillDiff + 65536 * 2) % 65536 > 49152) {
                tarAng = (int)lefthillAngle - (int)camYaw + (xoro_r(seed) % 80 - 70);
            }
            else {
                tarAng = (int)righthillAngle - (int)camYaw + (xoro_r(seed) % 80 - 10);
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
            int tarAng = (int)marioYawFacing - (int)camYaw;
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
        downhillAngle = ((int)(atan(pyraXNorm / pyraZNorm) * 32768.0 / M_PI)) % 65536;
    }
    int uphillDiff = (marioYawFacing - downhillAngle + 32768 + 65536 * 2) % 65536;
    //check for pbdr conditions
    if (fabs(pyraXNorm) + fabs(pyraZNorm) > .6 && marioHSpd >= 29.0 &&
        (uphillDiff < 6000 || uphillDiff > 59536) && xoro_r(seed) % 5 < 4 &&
        actTrunc == ACT_WALK) {
        //printf("did this\n");
        //printf("%f %f %f %d \n", pyraXNorm, pyraZNorm, marioHSpd, uphillDiff);
        int tarAng = (int)marioYawFacing - (int)camYaw + xoro_r(seed) % 14000 - 7000;
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
        in->x = (xoro_r(seed) % 256) - 128;
        in->y = (xoro_r(seed) % 256) - 128;
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
Vec3d truncFunc(float x, float y, float z, unsigned int marioAction, float marioHSpd, unsigned short marioYawFacing, unsigned short controlButDown, float bullyX, float bullyZ, uint8_t camMode,
    float pyraXNorm, float pyraYNorm, float pyraZNorm, float marioYVel) {
    uint64_t s = 0;
    unsigned int actTrunc = marioAction & 0x1FF;
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
    s += (int)((40 - marioYVel) / 4);

    //s *= 3;

    //fifd: Button press space mapped into 3 sections: no buttons, A only, anything else
    //if (actTrunc < 0x80 || actTrunc == 0x0A7) {
    //    if (controlButDown == 0)     s+= 1; // Pure nothing
    //    if (controlButDown == 32768) s+= 2; // Pure A
    //}

    //s *= 2;

    //if (actTrunc == 0x040 || actTrunc == 0x045) {
    //    if (camMode == 32) s+= 1; // Close cam
    //}

    /*s *= 40;
    s += (int)((pyraXNorm + 1)*20);

    s *= 40;
    s += (int)((pyraZNorm + 1)*20);*/

    float norm_regime_min = .69;
    //float norm_regime_max = .67;
    float target_xnorm = -.30725;
    float target_znorm = .3665;
    float x_delt = pyraXNorm - target_xnorm;
    float z_delt = pyraZNorm - target_znorm;
    float x_remainder = x_delt * 100 - floor(x_delt * 100);
    float z_remainder = z_delt * 100 - floor(z_delt * 100);


    //if((fabs(pyraXNorm) + fabs(pyraZNorm) < norm_regime_min) ||
    //   (fabs(pyraXNorm) + fabs(pyraZNorm) > norm_regime_max)){  //coarsen for bad norm regime
    if ((x_remainder > .001 && x_remainder < .999) || (z_remainder > .001 && z_remainder < .999) ||
        (fabs(pyraXNorm) + fabs(pyraZNorm) < norm_regime_min)) { //coarsen for not target norm envelope
        s *= 14;
        s += (int)((pyraXNorm + 1) * 7);

        s *= 14;
        s += (int)((pyraZNorm + 1) * 7);

        s += 1000000 + 1000000 * (int)floor((marioHSpd + 20) / 8);
        s += 100000000 * (int)floor((float)marioYawFacing / 16384.0);

        s *= 2;
        s += 1; //mark bad norm regime

        return Vec3d { (uint8_t)floor((x + 2330) / 200), (uint8_t)floor((y + 3200) / 400), (uint8_t)floor((z + 1090) / 200), s };
    }
    s *= 200;
    s += (int)((pyraXNorm + 1) * 100);

    float xzSum = fabs(pyraXNorm) + fabs(pyraZNorm);

    s *= 10;
    xzSum += (int)((xzSum - norm_regime_min) * 100);
    //s += (int)((pyraZNorm + 1)*100);

    s *= 30;
    s += (int)((pyraYNorm - .7) * 100);

    //fifd: Hspd mapped into sections {0-1, 1-2, ...}
    s += 30000000 + 30000000 * (int)floor((marioHSpd + 20));

    //fifd: Yaw mapped into sections
    //s += 100000000 * (int)floor((float)marioYawFacing / 2048.0);
    s += ((uint64_t)1200000000) * (int)floor((float)marioYawFacing / 4096.0);

    s *= 2; //mark good norm regime

    return Vec3d { (uint8_t)floor((x + 2330) / 10), (uint8_t)floor((y + 3200) / 50), (uint8_t)floor((z + 1090) / 10), s };
}

//fifd: Check equality of truncated states
int truncEq(Vec3d a, Vec3d b) {
    return (a.x == b.x) && (a.y == b.y) && (a.z == b.z) && (a.s == b.s);
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

uint64_t hashPos(Vec3d pos) {
    uint64_t tmpSeed = 0xCABBA6ECABBA6E;
    tmpSeed += pos.x + 0xCABBA6E;
    xoro_r(&tmpSeed);
    tmpSeed += pos.y + 0xCABBA6E;
    xoro_r(&tmpSeed);
    tmpSeed += pos.z + 0xCABBA6E;
    xoro_r(&tmpSeed);
    tmpSeed += pos.s + 0xCABBA6E;
    xoro_r(&tmpSeed);
    return tmpSeed;
}

int findNewHashInx(int* hashTab, int maxHashes, Vec3d pos) {
    uint64_t tmpSeed = hashPos(pos);
    for (int i = 0; i < 100; i++) {
        int inx = tmpSeed % maxHashes;
        if (hashTab[inx] == -1) return inx;
        xoro_r(&tmpSeed);
    }
    printf("Failed to find new hash index after 100 tries!\n");
    return -1;
}

//fifd: Given the data identifying the current block (pos), identify
//the index of that block in the hash table.
//Have not read this in detail or figured out the other arguments,
//but probably not important to understand
int findBlock(Block* blocks, int* hashTab, int maxHashes, Vec3d pos, int nMin, int nMax) {
    uint64_t tmpSeed = hashPos(pos);
    for (int i = 0; i < 100; i++) {
        int inx = tmpSeed % maxHashes;
        int blockInx = hashTab[inx];
        if (blockInx == -1) return nMax;
        if (blockInx >= nMin && blockInx < nMax) {
            if (truncEq(blocks[blockInx].pos, pos)) {
                return blockInx;
            }
        }
        xoro_r(&tmpSeed);
    }
    printf("Failed to find block from hash after 100 tries!\n");
    return -1; // TODO: Should be nMax?
}

void ParseArgs(int argc, char* argv[])
{
    strncpy(gProgName, argv[0], 128);
    for (int i = 0; i < 2; i++) {
        if (i >= argc) continue;
        printf("arg %d = %s\n", i, argv[i]);
        if (!strcmp(argv[i], "-silent")) {
            printf("Using silent mode.\n");
            gPrint = 0;
        }
    }
    printf("Not checking further args.\n");
}

void ScanDll()
{
    LPCWSTR dllBaseName = L"sm64_jp";
    LPCWSTR dllFullName = L"sm64_jp.dll";
    HMODULE testDLL = LoadLibrary(dllFullName);

    getDllInfo(testDLL);
    printf("Got DLL segments data %d %d bss %d %d\n", dataStart, dataLength, bssStart, bssLength);
}

class Configuration
{
public:
    int StartFrame;
    int SegmentLength;
    int MaxSegments;
    int MaxBlocks;
    int MaxHashes;
    int MaxSharedBlocks;
    int MaxSharedHashes;
    int TotalThreads;
    int MaxSharedSegments;
    int MaxLocalSegments;
};

class GlobalState
{
public:
    struct Segment** AllSegments;
    Block* AllBlocks;
    int* AllHashTabs;
    int* NBlocks;
    int* NSegments;
    Block* SharedBlocks;
    int* SharedHashTab;

    GlobalState(Configuration& config)
    {
        AllBlocks = (Block*)calloc(config.TotalThreads * config.MaxBlocks + config.MaxSharedBlocks, sizeof(Block));
        AllSegments = (struct Segment**)malloc((config.MaxSharedSegments + config.TotalThreads * config.MaxLocalSegments) * sizeof(struct Segment*));
        AllHashTabs = (int*)calloc(config.TotalThreads * config.MaxHashes + config.MaxSharedHashes, sizeof(int));
        NBlocks = (int*)calloc(config.TotalThreads + 1, sizeof(int));
        NSegments = (int*)calloc(config.TotalThreads + 1, sizeof(int));
        SharedBlocks = AllBlocks + config.TotalThreads * config.MaxBlocks;
        SharedHashTab = AllHashTabs + config.TotalThreads * config.MaxHashes;

        // Init shared hash table.
        for (int hashInx = 0; hashInx < config.MaxSharedHashes; hashInx++)
            SharedHashTab[hashInx] = -1; 
    }
};

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
}

void MergeBlocks(Configuration& config, GlobalState& gState)
{
    printfQ("Merging blocks.\n");

    int otid, n, m;
    for (otid = 0; otid < config.TotalThreads; otid++) {
        for (n = 0; n < gState.NBlocks[otid]; n++) {
            Block tmpBlock = gState.AllBlocks[otid * config.MaxBlocks + n];
            m = findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, tmpBlock.pos, 0, gState.NBlocks[config.TotalThreads]);
            if (m < gState.NBlocks[config.TotalThreads]) {
                if (tmpBlock.value > gState.SharedBlocks[m].value) { // changed to >
                    gState.SharedBlocks[m] = tmpBlock;
                }
            }
            else {
                gState.SharedHashTab[findNewHashInx(gState.SharedHashTab, config.MaxSharedHashes, tmpBlock.pos)] = gState.NBlocks[config.TotalThreads];
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

class ThreadState
{
public:
    Block* Blocks;
    int* HashTab;
    int Id;
    Block BaseBlock;

    ThreadState(Configuration& config, GlobalState& gState, int id)
    {
        Id = id;
        Blocks = gState.AllBlocks + Id * config.MaxBlocks;
        HashTab = gState.AllHashTabs + Id * config.MaxHashes;
    }

    void Initialize(Configuration& config, GlobalState& gState, Vec3d initTruncPos)
    {
        // Init local hash table.
        for (int hashInx = 0; hashInx < config.MaxHashes; hashInx++)
            HashTab[hashInx] = -1; 

        Blocks[0].pos = initTruncPos; //CHEAT TODO NOTE
        Blocks[0].tailSeg = (Segment*)malloc(sizeof(Segment)); //Instantiate root segment
        Blocks[0].tailSeg->numFrames = 0;
        Blocks[0].tailSeg->parent = NULL;
        Blocks[0].tailSeg->refCount = 0;
        Blocks[0].tailSeg->depth = 1;
        HashTab[findNewHashInx(HashTab, config.MaxHashes, Blocks[0].pos)] = 0;

        // Synchronize global state
        gState.AllSegments[gState.NSegments[Id] + Id * config.MaxLocalSegments] = Blocks[0].tailSeg;
        gState.NSegments[Id]++;
        gState.NBlocks[Id]++;
    }

    void SelectBaseBlock(GlobalState& gState)
    {

    }
};

/*
void ProcessNewBlock(Configuration& config, GlobalState& gState, int tid, Block& prevBlock, Vec3d newPos, float newFitness)
{
    Block newBlock;

    // Create and add block to list.
    if (gState.NBlocks[tid] == config.MaxBlocks) {
        printf("Max local blocks reached!\n");
    }
    else {
        //UPDATED FOR SEGMENTS STRUCT
        newBlock = prevBlock;
        newBlock.pos = newPos;
        newBlock.value = newFitness;
        int blInxLocal = findBlock(blocks, hashTab, config.MaxHashes, newPos, 0, gState.NBlocks[tid]);
        blInx = findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, newPos, 0, gState.NBlocks[config.TotalThreads]);

        if (blInxLocal < gState.NBlocks[tid]) { // Existing local block.
            if (newBlock.value >= blocks[blInxLocal].value) {
                Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                newSeg->parent = origBlock.tailSeg;
                newSeg->refCount = 0;
                newSeg->numFrames = f + 1;
                newSeg->seed = origSeed;
                newSeg->depth = origBlock.tailSeg->depth + 1;
                if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                if (origBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                newBlock.tailSeg = newSeg;
                gState.AllSegments[tid * config.MaxLocalSegments + gState.NSegments[tid]] = newSeg;
                gState.NSegments[tid] += 1;
                blocks[blInxLocal] = newBlock;
            }
        }
        else if (blInx < gState.NBlocks[config.TotalThreads] && newBlock.value < gState.SharedBlocks[blInx].value);// Existing shared block but worse.
        else { // Existing shared block and better OR completely new block.
            hashTab[findNewHashInx(hashTab, config.MaxHashes, newPos)] = gState.NBlocks[tid];
            Segment* newSeg = (Segment*)malloc(sizeof(Segment));
            newSeg->parent = origBlock.tailSeg;
            newSeg->refCount = 1;
            newSeg->numFrames = f + 1;
            newSeg->seed = origSeed;
            newSeg->depth = origBlock.tailSeg->depth + 1;
            if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
            if (origBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
            newBlock.tailSeg = newSeg;
            gState.AllSegments[tid * config.MaxLocalSegments + gState.NSegments[tid]] = newSeg;
            gState.NSegments[tid] += 1;
            blocks[gState.NBlocks[tid]++] = newBlock;
        }
    }
}
*/

void main(int argc, char* argv[]) {
    ParseArgs(argc, argv);

    ScanDll();

    Configuration config;
    InitConfiguration(config);

    GlobalState gState = GlobalState(config);

    omp_set_num_threads(config.TotalThreads);
    #pragma omp parallel
    {
        ThreadState tState = ThreadState(config, gState, omp_get_thread_num());
        printf("Thread %d\n", tState.Id);

        //TODO: revert hardcoding
        LPCWSTR dlls[4] = {L"sm64_jp_0.dll", L"sm64_jp_1.dll" , L"sm64_jp_2.dll" , L"sm64_jp_3.dll" };
        HMODULE hDLL = LoadLibrary(dlls[tState.Id]);

        // Functions
        VOIDFUNC sm64_init = (VOIDFUNC)GetProcAddress(hDLL, "sm64_init");
        VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(hDLL, "sm64_update");
        GFXFUNC  envfx_update_particles = (GFXFUNC)GetProcAddress(hDLL, "envfx_update_particles");

        // Variables
        Input* gControllerPads = (Input*)GetProcAddress(hDLL, "gControllerPads");
        void* gMarioStates = GetProcAddress(hDLL, "gMarioStates");
        void* gObjectPool = GetProcAddress(hDLL, "gObjectPool");
        void* gEnvironmentRegions = GetProcAddress(hDLL, "gEnvironmentRegions");
        void* gCamera = GetProcAddress(hDLL, "gCamera");
        void* gLakituState = GetProcAddress(hDLL, "gLakituState");
        unsigned char* gLastCompletedStarNum = (unsigned char*)GetProcAddress(hDLL, "gLastCompletedStarNum");
        short* gCurrCourseNum = (short*)GetProcAddress(hDLL, "gCurrCourseNum");
        short* gCurrAreaIndex = (short*)GetProcAddress(hDLL, "gCurrAreaIndex");
        void* gControllers = (void*)GetProcAddress(hDLL, "gControllers");
        uint32_t* gTimeStopState = (uint32_t*)GetProcAddress(hDLL, "gTimeStopState");
        uint8_t* redCnt = (uint8_t*)GetProcAddress(hDLL, "gRedCoinsCollected");

        short* starCnt = (short*)((char*)gMarioStates + 230);
        float* marioX = (float*)((char*)gMarioStates + 60);
        float* marioY = (float*)((char*)gMarioStates + 64);
        float* marioZ = (float*)((char*)gMarioStates + 68);
        float* marioFloorHeight = (float*)((char*)gMarioStates + 0x07C);
        float* marioHSpd = (float*)((char*)gMarioStates + 0x54);
        uint16_t* marioYawFacing = (uint16_t*)((char*)gMarioStates + 46);
        short* marioYawVel = (short*)((char*)gMarioStates + 52);
        float* marioYVel = (float*)((char*)gMarioStates + 76);
        short* marioPitch = (short*)((char*)gMarioStates + 44);
        short* marioPitchVel = (short*)((char*)gMarioStates + 50);
        uint16_t* camYaw = (uint16_t*)((char*)gCamera + 340);
        uint8_t* camMode = (uint8_t*)hDLL + bssStart + 29605;
        unsigned int* marioAction = (unsigned int*)((char*)gMarioStates + 12);
        unsigned int* prevAction = (unsigned int*)((char*)gMarioStates + 0x10);
        unsigned short* actionTimer = (unsigned short*)((char*)gMarioStates + 0x1A);
        unsigned short* controlButDown = (unsigned short*)((char*)gControllers + 0x10);
        uint32_t* ringCnt = (uint32_t*)((char*)gObjectPool + 18 * 1392 + 508);
        char* firstCol = (char*)((char*)gObjectPool + 30 * 1392 + 412);

        float* pyraXNorm = (float*)((char*)gObjectPool + 84 * 1392 + 324);
        float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);
        float* pyraZNorm = (float*)((char*)gObjectPool + 84 * 1392 + 332);

        float* bullyX = (float*)((char*)gObjectPool + 57 * 1392 + 56);
        float* bullyY = (float*)((char*)gObjectPool + 57 * 1392 + 60);
        float* bullyZ = (float*)((char*)gObjectPool + 57 * 1392 + 64);

        const int sumFramesBack = 12;

        sm64_init();

        int printingDRLand = 1;

        uint64_t seed = (uint64_t)(tState.Id + 173) * 5786766484692217813;
        //uint64_t seed = (uint64_t)(tid + 173) * time(NULL);
        double timerStart, loadTime = 0, runTime = 0, blockTime = 0;

        // Read inputs from file
        Input* fileInputs = GetM64("C:\\Users\\Tyler\\Documents\\repos\\uscx\\x64\\Debug\\4_units_from_edge.m64");

        // Run the inputs.
        int startCourse = 0, startArea = 0;
        Input* m64short = (Input*)malloc(sizeof(Input) * (config.SegmentLength * config.MaxSegments + 256)); // Todo: Nasty

        int maxLightning = 10000;
        int lightLen = 0;
        Vec3d* lightningList = (Vec3d*)malloc(sizeof(Vec3d) * maxLightning);
        Vec3d* lightningLocal = (Vec3d*)malloc(sizeof(Vec3d) * maxLightning);

        SaveState state, state2;
        allocState(&state);
        allocState(&state2);

        Input in;
        for (int f = 0; f < config.StartFrame + 5; f++) {
            *gControllerPads = in = fileInputs[f];
            sm64_update();
            if (f == config.StartFrame - 1) save(hDLL, &state);
            if (f == config.StartFrame - 1) {
                startCourse = *gCurrCourseNum;
                startArea = *gCurrAreaIndex;
            }
        }

        // Give info to the root block.
        riskyLoadJ(hDLL, &state);
        Vec3d initTruncPos = truncFunc(*marioX, *marioY, *marioZ, *marioAction,
            *marioHSpd, *marioYawFacing, *marioPitch, *marioPitchVel, *controlButDown,
            *camMode, *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel);

        tState.Initialize(config, gState, initTruncPos);

        double pureStart = omp_get_wtime();

        for (int mainLoop = 0; mainLoop <= 1000000000; mainLoop++) {
            int blInx, seg, trueF, origInx;
            Block origBlock, newBlock;
            uint64_t tmpSeed;

            // ALWAYS START WITH A MERGE SO THE SHARED BLOCKS ARE OK.
            if (mainLoop % 300 == 0) {
                //if (mainLoop % 5 == 0) {
                #pragma omp barrier
                if (tState.Id == 0) {
                    // Merge all blocks from all threads and redistribute info.
                    MergeBlocks(config, gState);

                    // Handle segments
                    MergeSegments(config, gState);

                    if (mainLoop % 3000 == 0) 
                        SegmentGarbageCollection(config, gState);

                    printfQ("\nThread ALL Loop %d blocks %d\n", mainLoop, gState.NBlocks[config.TotalThreads]);
                    printfQ("LOAD %.3f RUN %.3f BLOCK %.3f TOTAL %.3f\n", loadTime, runTime, blockTime, omp_get_wtime() - pureStart);
                    pureStart = omp_get_wtime();
                    loadTime = runTime = blockTime = 0;
                    printfQ("\n\n");
                }
                flushLog();
                #pragma omp barrier
            }

            // Pick a block.
            origInx = gState.NBlocks[config.TotalThreads];
            if (mainLoop % 15 == 0) {
                origInx = 0;
            }
            else if (mainLoop % 7 == 1 && lightLen > 0) {
                for (int attempt = 0; attempt < 1000; attempt++) {
                    int randomLightInx = xoro_r(&seed) % lightLen;
                    origInx = findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, lightningList[randomLightInx], 0, gState.NBlocks[config.TotalThreads]);
                    if (origInx != gState.NBlocks[config.TotalThreads]) break;
                }
                if (origInx == gState.NBlocks[config.TotalThreads]) {
                    printf("Could not find lightning block, using root!\n");
                    origInx = 0;
                }
            }
            else {
                int weighted = xoro_r(&seed) % 5;
                for (int attempt = 0; attempt < 100000; attempt++) {
                    origInx = xoro_r(&seed) % gState.NBlocks[config.TotalThreads];
                    if (gState.SharedBlocks[origInx].tailSeg == 0) { printf("Chosen block tailseg null!\n"); continue; }
                    if (gState.SharedBlocks[origInx].tailSeg->depth == 0) { printf("Chosen block tailseg depth 0!\n"); continue; }
                    uint64_t s = gState.SharedBlocks[origInx].pos.s;
                    int normInfo = s % 900;
                    float xNorm = (float)((int)normInfo / 30);
                    float zNorm = (float)(normInfo % 30);
                    float approxXZSum = fabs((xNorm - 15) / 15) + fabs((zNorm - 15) / 15) + .01;
                    if (((float)(xoro_r(&seed) % 50) / 100 < approxXZSum * approxXZSum) & (gState.SharedBlocks[origInx].tailSeg->depth < config.MaxSegments)) break;
                }
                if (origInx == gState.NBlocks[config.TotalThreads]) {
                    printf("Could not find block!\n");
                    break;
                }
            }

            origBlock = gState.SharedBlocks[origInx];
            if (origBlock.tailSeg->depth > config.MaxSegments + 2) { printf("origBlock depth above max!\n"); }
            if (origBlock.tailSeg->depth == 0) { printf("origBlock depth is zero!\n"); }

            // Create a state for this block.
            //load(hDLL, &state);
            //riskyLoad(hDLL, &state);
            riskyLoadJ(hDLL, &state);
            //riskyLoad2(hDLL, &state);
            trueF = 0;

            int lightLenLocal = 0;
            Vec3d newPos = truncFunc(*marioX, *marioY, *marioZ, *marioAction, *marioHSpd, *marioYawFacing, *marioPitch, *marioPitchVel, *controlButDown, *camMode, *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel);
            lightningLocal[lightLenLocal++] = newPos;

            //UPDATED FOR SEGMENTS STRUCT
            //Before, I temporarily reversed the linked list.
            //But this doesn't work in a multi threaded environment.
            //Punt and do this in quadratic time. This shouldn't be a
            //bottleneck anyway but can fix it if needed
            if (origBlock.tailSeg == 0)printf("origBlock has null tailSeg");
            Segment* thisTailSeg = origBlock.tailSeg;
            Segment* curSeg;
            int thisSegDepth = thisTailSeg->depth;
            float prevXZSums[sumFramesBack];
            float prevHSpds[sumFramesBack];
            memset(prevXZSums, 0, sizeof(prevXZSums));
            memset(prevHSpds, 0, sizeof(prevHSpds));

            for (int i = 1; i <= thisSegDepth; i++) {
                curSeg = thisTailSeg;
                while (curSeg->depth != i) {  //inefficient but probably doesn't matter
                    if (curSeg->parent == 0)printf("Parent is null!");
                    if (curSeg->parent->depth + 1 != curSeg->depth) { printf("Depths wrong"); }
                    curSeg = curSeg->parent;
                }
                //Run the inputs
                tmpSeed = curSeg->seed;
                int megaRandom = xoro_r(&tmpSeed) % 2;
                for (int f = 0; f < curSeg->numFrames; f++) {
                    perturbInput(&in, &tmpSeed, trueF, *marioAction, *marioYawFacing, *pyraXNorm, *pyraZNorm, *marioHSpd, *camYaw, megaRandom);
                    m64short[trueF++] = in;
                    *gControllerPads = in;
                    sm64_update();
                    //updateValue(&value);
                    float xzSum = fabs(*pyraXNorm) + fabs(*pyraZNorm);
                    for (int prevXZSumInd = 0; prevXZSumInd < sumFramesBack - 1; prevXZSumInd++) {
                        prevXZSums[prevXZSumInd] = prevXZSums[prevXZSumInd + 1];
                        prevHSpds[prevXZSumInd] = prevHSpds[prevXZSumInd + 1];
                    }
                    prevXZSums[sumFramesBack - 1] = 0;
                    prevHSpds[sumFramesBack - 1] = 0;
                    unsigned int actionTrunc = *marioAction & 0x1FF;
                    if (*marioHSpd > 0 && actionTrunc == 0x040) {
                        prevXZSums[sumFramesBack - 1] = xzSum;
                        prevHSpds[sumFramesBack - 1] = *marioHSpd;
                    }
                    newPos = truncFunc(*marioX, *marioY, *marioZ, *marioAction, *marioHSpd, *marioYawFacing, *marioPitch, *marioPitchVel, *controlButDown, *camMode, *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel);
                    if (!truncEq(newPos, lightningLocal[lightLenLocal - 1])) {
                        if (lightLenLocal < maxLightning) {
                            lightningLocal[lightLenLocal++] = newPos;
                        }
                        else {
                            printf("Reached max lightning!\n");
                        }
                    }
                }
            }

            save(hDLL, &state2);
            //xorStates(&state, &state2, xorSlaves + tid, tid);
            Input origLastIn = in;
            Vec3d origPos = truncFunc(*marioX, *marioY, *marioZ, *marioAction, *marioHSpd, *marioYawFacing, *marioPitch, *marioPitchVel, *controlButDown, *camMode, *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel);
            //int   origValue = value;
            int   origLightLenLocal = lightLenLocal;
            if (!truncEq(origBlock.pos, origPos)) {
                printf("ORIG %d %d %d %ld AND BLOCK %d %d %d %ld NOT EQUAL\n", origPos.x, origPos.y, origPos.z, origPos.s, origBlock.pos.x, origBlock.pos.y, origBlock.pos.z, origBlock.pos.s);
                Segment* curSegDebug = origBlock.tailSeg;
                while (curSegDebug != 0) {  //inefficient but probably doesn't matter
                    if (curSegDebug->parent == 0)printf("Parent is null!");
                    if (curSegDebug->parent->depth + 1 != curSegDebug->depth) { printf("Depths wrong"); }
                    curSegDebug = curSegDebug->parent;
                }
            }

            // From state run a bunch of times.
            int subLoopMax = 200;
            if (mainLoop == 0) subLoopMax = 200;
            for (int subLoop = 0; subLoop < subLoopMax; subLoop++) {
                float finishTime = 0.0;
                //int newBigBoy = 0;
                Vec3d oldPos;
                uint64_t origSeed = seed;
                timerStart = omp_get_wtime();
                //load(hDLL, &state2);
                //riskyLoad(hDLL, &state2);
                riskyLoadJ(hDLL, &state2);
                //riskyLoad2(hDLL, &state2);
                loadTime += omp_get_wtime() - timerStart;
                oldPos = origPos;
                in = origLastIn;
                lightLenLocal = origLightLenLocal;

                int megaRandom = xoro_r(&seed) % 2;

                int maxRun = config.SegmentLength;
                for (int f = 0; f < maxRun; f++) {
                    perturbInput(&in, &seed, trueF + f, *marioAction, *marioYawFacing, *pyraXNorm, *pyraZNorm, *marioHSpd, *camYaw, megaRandom);
                    m64short[trueF + f] = in;
                    *gControllerPads = in;
                    timerStart = omp_get_wtime();
                    sm64_update();
                    runTime += omp_get_wtime() - timerStart;

                    unsigned int actionTrunc = *marioAction & 0x1FF;
                    newPos = truncFunc(*marioX, *marioY, *marioZ, *marioAction, *marioHSpd, *marioYawFacing, *marioPitch, *marioPitchVel, *controlButDown, *camMode, *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel);

                    //if ((*gTimeStopState || actionTrunc == 0x104) && maxRun < 200) maxRun++;

                    if (*gCurrCourseNum != startCourse || *gCurrAreaIndex != startArea) break;

                    //fifd: This is the check for the final objective function
                    //to be minimized.
                    //island, new idea
                    float xzSum = fabs(*pyraXNorm) + fabs(*pyraZNorm);
                    for (int prevXZSumInd = 0; prevXZSumInd < sumFramesBack - 1; prevXZSumInd++) {
                        prevXZSums[prevXZSumInd] = prevXZSums[prevXZSumInd + 1];
                        prevHSpds[prevXZSumInd] = prevHSpds[prevXZSumInd + 1];
                    }
                    prevXZSums[sumFramesBack - 1] = 0;
                    prevHSpds[sumFramesBack - 1] = 0;
                    if (*marioHSpd > 0 && actionTrunc == 0x040) {
                        prevXZSums[sumFramesBack - 1] = xzSum;
                        prevHSpds[sumFramesBack - 1] = *marioHSpd;
                    }
                    //printf("%f %f\n", *pyraXNorm, *pyraZNorm);
                    //printf("%f %f\n", xzSum, bestTimes[tid][0]);



                    if (*marioX < -2330) break;
                    if (*marioX > -1550) break;
                    if (*marioZ < -1090) break;
                    if (*marioZ > -300) break;
                    if (*marioY > -2760) break;
                    if (*pyraZNorm < -.15 || *pyraXNorm > 0.15) break; //stay in desired quadrant
                    if (actionTrunc != ACT_BRAKE && actionTrunc != ACT_DIVE && actionTrunc != ACT_DIVE_LAND &&
                        actionTrunc != ACT_DR && actionTrunc != ACT_DR_LAND && actionTrunc != ACT_FREEFALL &&
                        actionTrunc != ACT_FREEFALL_LAND && actionTrunc != ACT_TURNAROUND_1 &&
                        actionTrunc != ACT_TURNAROUND_2 && actionTrunc != ACT_WALK) {
                        break;
                    } //not useful action, such as lava boost
                    if (actionTrunc == ACT_FREEFALL && *marioYVel > -20.0) break;//freefall without having done nut spot chain
                    if (*marioFloorHeight > -3071 && *marioY > *marioFloorHeight + 4 &&
                        *marioYVel != 22.0) break;//above pyra by over 4 units
                    if (*marioFloorHeight == -3071 && actionTrunc != ACT_FREEFALL) break; //diving/dring above lava



                    if (actionTrunc == ACT_DR && fabs(*pyraXNorm) > .3 && fabs(*pyraXNorm) + fabs(*pyraZNorm) > .65 &&
                        *marioX + *marioZ > (-1945 - 715)) {  //make sure Mario is going toward the right/east edge
                        char fileName[128];
                        //printf("dr\n");
                        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\uscx\\x64\\Debug\\m64s\\dr\\bitfs_dr_%f_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel, tState.Id);
                        writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\uscx\\x64\\Debug\\4_units_from_edge.m64", m64short, config.StartFrame, trueF + f + 1);
                    }
                    //check on hspd > 1 confirms we're in dr land rather than quickstopping,
                    //which gives the same action
                    if (actionTrunc == ACT_DR_LAND && *marioY > -2980 && *marioHSpd > 1
                        && fabs(*pyraXNorm) > .29 && fabs(*marioX) > -1680) {
                        char fileName[128];
                        //if(printingDRLand > 0)printf("dr land\n");
                        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\uscx\\x64\\Debug\\m64s\\drland\\bitfs_drland_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, tState.Id);
                        writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\uscx\\x64\\Debug\\4_units_from_edge.m64", m64short, config.StartFrame, trueF + f + 1);
                    }

                    if (!truncEq(newPos, lightningLocal[lightLenLocal - 1])) {
                        if (lightLenLocal < maxLightning) {
                            lightningLocal[lightLenLocal++] = newPos;
                        }
                        else {
                            printf("Reached max lightning!\n");
                        }
                    }

                    //fifd: Checks to see if we're in a new Block. If so, save off the segment so far.
                    timerStart = omp_get_wtime();
                    if (actionTrunc < 0xC0 && !truncEq(newPos, oldPos) && !truncEq(newPos, origPos)) {

                        // Create and add block to list.
                        if (gState.NBlocks[tState.Id] == config.MaxBlocks) {
                            printf("Max local blocks reached!\n");
                        }
                        else {
                            //UPDATED FOR SEGMENTS STRUCT
                            newBlock = origBlock;
                            newBlock.pos = newPos;
                            newBlock.value = -fabs(*pyraYNorm);
                            int blInxLocal = findBlock(tState.Blocks, tState.HashTab, config.MaxHashes, newPos, 0, gState.NBlocks[tState.Id]);
                            blInx = findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, newPos, 0, gState.NBlocks[config.TotalThreads]);

                            if (blInxLocal < gState.NBlocks[tState.Id]) { // Existing local block.
                                if (newBlock.value >= tState.Blocks[blInxLocal].value) {
                                    Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                                    newSeg->parent = origBlock.tailSeg;
                                    newSeg->refCount = 0;
                                    newSeg->numFrames = f + 1;
                                    newSeg->seed = origSeed;
                                    newSeg->depth = origBlock.tailSeg->depth + 1;
                                    if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                                    if (origBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                                    newBlock.tailSeg = newSeg;
                                    gState.AllSegments[tState.Id * config.MaxLocalSegments + gState.NSegments[tState.Id]] = newSeg;
                                    gState.NSegments[tState.Id] += 1;
                                    tState.Blocks[blInxLocal] = newBlock;
                                }
                            }
                            else if (blInx < gState.NBlocks[config.TotalThreads] && newBlock.value < gState.SharedBlocks[blInx].value);// Existing shared block but worse.
                            else { // Existing shared block and better OR completely new block.
                                tState.HashTab[findNewHashInx(tState.HashTab, config.MaxHashes, newPos)] = gState.NBlocks[tState.Id];
                                Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                                newSeg->parent = origBlock.tailSeg;
                                newSeg->refCount = 1;
                                newSeg->numFrames = f + 1;
                                newSeg->seed = origSeed;
                                newSeg->depth = origBlock.tailSeg->depth + 1;
                                if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                                if (origBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                                newBlock.tailSeg = newSeg;
                                gState.AllSegments[tState.Id * config.MaxLocalSegments + gState.NSegments[tState.Id]] = newSeg;
                                gState.NSegments[tState.Id] += 1;
                                tState.Blocks[gState.NBlocks[tState.Id]++] = newBlock;
                            }
                        }
                        oldPos = newPos; // TTODO: Why this here?
                    }
                    blockTime += omp_get_wtime() - timerStart;
                }

                //Now clear out sums array since we'll be making a new
                //segment next.
                for (int prevXZSumInd = 0; prevXZSumInd < sumFramesBack; prevXZSumInd++) {
                    prevXZSums[prevXZSumInd] = 0;
                    prevHSpds[prevXZSumInd] = 0;
                }
            }
        }
    }
}
