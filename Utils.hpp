#pragma once
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

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <math.h>

#ifndef UTILS_H
#define UTILS_H

typedef void (CALLBACK* VOIDFUNC)();
typedef void* (CALLBACK* GFXFUNC)(int, void*, void*, void*);

typedef struct {
    unsigned short b;
    char x;
    char y;
} Input;

class Utils
{
public:
    //fifd: only called by xoro_r
    static const uint32_t rotl(const uint32_t x, int k) {
        return (x << k) | (x >> (32 - k));
    }

    //fifd: random number generation utility
    static uint32_t xoro_r(uint64_t* s) {
        const uint32_t s0 = *s >> 32;
        uint32_t s1 = (uint32_t)(*s) ^ s0;
        *s = (uint64_t)(rotl(s0, 26) ^ s1 ^ (s1 << 9)) << 32 | rotl(s1, 13);
        return rotl(s0 * 0x9E3779BB, 5) * 5;
    }

    static void writeFile(char* newFile, const char* base, Input* inputs, int offset, int length) {
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

    static void copyDll(char* newFile, char* base) {
        FILE* fp1 = fopen(base, "rb");
        FILE* fp2 = fopen(newFile, "wb");
        unsigned char a;
        while (fread(&a, 1, 1, fp1)) fwrite(&a, 1, 1, fp2);
        fclose(fp1);
        fclose(fp2);
    }

    template <typename F>
    static void MultiThread(int nThreads, F func)
    {
        omp_set_num_threads(nThreads);
        #pragma omp parallel
        {
            func();
        }
    }


    template <typename F>
    static void SingleThread(F func)
    {
        #pragma omp barrier
        {
            if (omp_get_thread_num() == 0)
                func();
        }
        #pragma omp barrier

        return;
    }

    static float dist3d(float a, float b, float c, float x, float y, float z) {
        return sqrt((a - x) * (a - x) + (b - y) * (b - y) + (c - z) * (c - z));
    }

    static int angDist(unsigned short a, unsigned short b) {
        int dist = abs((int)a - (int)b);
        if (dist > 32768) dist = 65536 - dist;
        return dist;
    }

    static int vibeCheck(float tarX, float tarZ, float tarT, float spd, float curX, float curZ, float curT) {
        float dist = sqrt((curX - tarX) * (curX - tarX) + (curZ - tarZ) * (curZ - tarZ));
        if (dist < (tarT - curT)* spd) return 1;
        return 0;
    }

    static int leftLine(float a, float b, float c, float d, float e, float f) {
        return (b - d) * (e - a) + (c - a) * (f - b) > 0 ? 0 : 1;
    }

    static Input* GetM64(const char* path)
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

    static const char dataMap[8192];
    static const char bssMap[8192];
};



class Dll
{
public:
    HMODULE hdll;

    int dataStart, dataLength, bssStart, bssLength;

    Dll(LPCWSTR path)
    {
        getDllInfo(path);
    }

    void getDllInfo(LPCWSTR path) {
        hdll = LoadLibrary(path);

        IMAGE_NT_HEADERS* pNtHdr = ImageNtHeader(hdll);
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

        printf("Got DLL segments data %d %d bss %d %d\n", dataStart, dataLength, bssStart, bssLength);
    }
};

class SaveState {
public:
    void* data;
    void* bss;

    void allocState(Dll& dll) {
        data = calloc(dll.dataLength, 1);
        bss = calloc(dll.bssLength, 1);
    }

    void allocStateSmall(Dll& dll) {
        data = calloc(10000, 1);
        bss = calloc(dll.bssLength, 1);
    }

    void freeState() {
        free(data);
        free(bss);
        data = bss = NULL;
    }

    void load(Dll& dll) {
        memcpy((char*)dll.hdll + dll.dataStart, (char*)data, dll.dataLength);
        memcpy((char*)dll.hdll + dll.bssStart, (char*)bss, dll.bssLength);
    }

    void riskyLoad(Dll& dll) {
        memcpy((char*)dll.hdll + dll.dataStart + 0, (char*)data + 0, 100000);
        memcpy((char*)dll.hdll + dll.dataStart + 25 * 100000, (char*)data + 25 * 100000, 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 0, (char*)bss + 0, 6 * 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 17 * 100000, (char*)bss + 17 * 100000, 6 * 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 47 * 100000, (char*)bss + 47 * 100000, 100000);
    }

    void riskyLoad2(Dll& dll) {
        int off = 0;
        for (int i = 0; i < strlen(Utils::dataMap); i++) {
            if (Utils::dataMap[i] == 'X') {
                memcpy((char*)dll.hdll + dll.dataStart + off, (char*)data + off, 1000);
                off += 1000;
            }
            if (Utils::dataMap[i] == '.') off += 1000;
        }
        off = 0;
        for (int i = 0; i < strlen(Utils::bssMap); i++) {
            if (Utils::bssMap[i] == 'X') {
                if (!(off < 400000 && off >= 28000)) {
                    memcpy((char*)dll.hdll + dll.bssStart + off, (char*)bss + off, 1000);
                }
                off += 1000;
            }
            if (Utils::bssMap[i] == '.') off += 1000;
        }
        memcpy((char*)dll.hdll + dll.bssStart + 28000, (char*)bss + 28000, 372000);
        memcpy((char*)dll.hdll + dll.bssStart + 4742000, (char*)bss + 4742000, 1000);
    }

    void save(Dll& dll) {
        memcpy((char*)data, (char*)dll.hdll + dll.dataStart, dll.dataLength);
        memcpy((char*)bss, (char*)dll.hdll + dll.bssStart, dll.bssLength);
    }

    double riskyLoadJ(Dll& dll) {
        auto timerStart = omp_get_wtime();

        memcpy((char*)dll.hdll + dll.dataStart + 0, (char*)data + 0, 100000);
        memcpy((char*)dll.hdll + dll.dataStart + 20 * 100000, (char*)data + 20 * 100000, 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 0, (char*)bss + 0, 6 * 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 17 * 100000, (char*)bss + 17 * 100000, 6 * 100000);
        memcpy((char*)dll.hdll + dll.bssStart + 47 * 100000, (char*)bss + 47 * 100000, 100000);

        return omp_get_wtime() - timerStart;
    }
};

class Printer
{
public:
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
};



#endif
