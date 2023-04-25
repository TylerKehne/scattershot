#pragma once

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