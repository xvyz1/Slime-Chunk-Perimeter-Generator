#include "Perimeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RADIUS 152
#define AFK_ABOVE_FLOOR 14
#define VOID_ABOVE_FLOOR 24

const char *const VERSION_NAMES[VERSION_COUNT] = {
    "1.13 - 1.17",
    "1.18 - 1.20.4",
    "1.20.5 - 1.21.10",
    "1.21.11 and later"
};

const char *const VERSION_TAGS[VERSION_COUNT] = {
    "1.13-1.17",
    "1.18-1.20.4",
    "1.20.5-1.21.10",
    "1.21.11+"
};

static int floorLevel(int version) { return version == VERSION_1_13 ? 0 : -64; }

static int64_t chunkOf(int64_t block) {
    return block >= 0 ? block / 16 : -((-block + 15) / 16);
}

static int isSlimeChunk(int64_t worldSeed, int64_t chunkX, int64_t chunkZ) {
    uint64_t seed = (uint64_t)worldSeed;
    seed += (uint64_t)(int64_t)(int32_t)(chunkX * chunkX * 0x4c1906LL);
    seed += (uint64_t)(int64_t)(int32_t)(chunkX * 0x5ac0dbLL);
    seed += (uint64_t)((int64_t)((int32_t)(chunkZ * chunkZ)) * 0x4307a7LL);
    seed += (uint64_t)(int64_t)(int32_t)(chunkZ * 0x5f24fLL);
    seed ^= 0x3ad8025fULL;

    uint64_t state = (seed ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    for (;;) {
        state = (state * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        int32_t bits = (int32_t)(state >> 17);
        int32_t val = bits % 10;
        if (bits - val + 9 >= 0) return val == 0;
    }
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int count;
    int failed;
} Monolith;


static void append(Monolith *m, const char *s) {
    if (m->failed) return;
    size_t n = strlen(s);
    if (m->len + n + 1 > m->cap) {
        size_t cap = m->cap ? m->cap : 1 << 16;
        while (m->len + n + 1 > cap) cap *= 2;
        char *p = (char *)realloc(m->data, cap);
        if (!p) { m->failed = 1; return; }
        m->data = p;
        m->cap = cap;
    }
    memcpy(m->data + m->len, s, n + 1);
    m->len += n;
}

static void monolithStart(Monolith *m) {
    memset(m, 0, sizeof *m);
    append(m, "summon falling_block ~ ~1 ~ {BlockState:{Name:\"minecraft:redstone_block\"},Time:1,Passengers:"
                "[{id:\"minecraft:falling_block\",BlockState:{Name:\"minecraft:activator_rail\"},Time:1,Passengers:[");
}

static void monolithAdd(Monolith *m, const char *command) {
    char piece[256];
    snprintf(piece, sizeof piece, "%s{id:\"minecraft:command_block_minecart\",Command:\"%s\"}",
             m->count ? "," : "", command);
    append(m, piece);
    m->count++;
}

static void monolithEnd(Monolith *m) {
    append(m, ",{id:\"minecraft:command_block_minecart\",Command:\"setblock ~ ~ ~ air\"}"
                ",{id:\"minecraft:command_block_minecart\",Command:\"setblock ~ ~-1 ~ air\"}"
                ",{id:\"minecraft:command_block_minecart\",Command:\"kill @e[type=command_block_minecart,distance=..3]\"}]}]}");
}

static void giveCommand(char *out, size_t n, int version) {
    if (version <= VERSION_1_18)
        snprintf(out, n, "/give @p minecraft:repeating_command_block"
                         "{BlockEntityTag:{Command:\"execute at @p run perimeterinfo\",auto:1b}}");
    else
        snprintf(out, n, "/give @p repeating_command_block[block_entity_data="
                         "{id:\"minecraft:command_block\",Command:\"execute at @p run perimeterinfo\",auto:1b}]");
}

int generatePerimeter(const Options *opt, const char *path, Report *rep) {
    const int64_t cxMin = chunkOf(opt->x - RADIUS), cxMax = chunkOf(opt->x + RADIUS);
    const int64_t czMin = chunkOf(opt->z - RADIUS), czMax = chunkOf(opt->z + RADIUS);
    const int nx = (int)(cxMax - cxMin + 1), nz = (int)(czMax - czMin + 1);

    const int floorY = floorLevel(opt->version), afkY = floorY + AFK_ABOVE_FLOOR, voidTop = floorY + VOID_ABOVE_FLOOR;
    const int64_t bx0 = cxMin * 16 - 1, bz0 = czMin * 16 - 1;
    const int w = nx * 16 + 2, h = nz * 16 + 2;

    unsigned char *slime = (unsigned char *)calloc((size_t)nx * nz, 1);
    unsigned char *border = (unsigned char *)calloc((size_t)w * h, 1);
    Monolith floorCmds, borderCmds;
    monolithStart(&floorCmds);
    monolithStart(&borderCmds);

    if (!slime || !border || floorCmds.failed || borderCmds.failed) {
        free(slime); free(border); free(floorCmds.data); free(borderCmds.data);
        return 0;
    }

    int slimeCount = 0;
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++)
            if (isSlimeChunk(opt->seed, cxMin + i, czMin + j)) { slime[i * nz + j] = 1; slimeCount++; }

    for (int j = 0; j < nz; j++)
        for (int i = 0; i < nx; i++)
            if (slime[i * nz + j]) {
                const int64_t x0 = (cxMin + i) * 16, z0 = (czMin + j) * 16;
                char line[160];
                snprintf(line, sizeof line, "fill %lld %d %lld %lld %d %lld obsidian",
                         (long long)x0, floorY, (long long)z0,
                         (long long)(x0 + 15), floorY, (long long)(z0 + 15));
                monolithAdd(&floorCmds, line);
            }

    long long borderBlocks = 0;
    for (int ix = 0; ix < w; ix++) {
        const int ci = (int)chunkOf(ix - 1);
        for (int iz = 0; iz < h; iz++) {
            const int cj = (int)chunkOf(iz - 1);
            if (ci >= 0 && ci < nx && cj >= 0 && cj < nz && slime[ci * nz + cj]) continue;
            int touches = 0;
            for (int dx = -1; dx <= 1 && !touches; dx++)
                for (int dz = -1; dz <= 1 && !touches; dz++) {
                    if (!dx && !dz) continue;
                    const int ni = (int)chunkOf(ix + dx - 1), nj = (int)chunkOf(iz + dz - 1);
                    if (ix + dx < 0 || ix + dx >= w || iz + dz < 0 || iz + dz >= h) continue;
                    if (ni >= 0 && ni < nx && nj >= 0 && nj < nz && slime[ni * nz + nj]) touches = 1;
                }
            if (touches) { border[ix * h + iz] = 1; borderBlocks++; }
        }
    }

    for (int ix = 0; ix < w; ix++)
        for (int iz = 0; iz < h; iz++) {
            if (!border[ix * h + iz]) continue;
            int rw = 1;
            while (ix + rw < w && border[(ix + rw) * h + iz]) rw++;
            int rh = 1, extend = 1;
            while (extend && iz + rh < h) {
                for (int k = 0; k < rw; k++)
                    if (!border[(ix + k) * h + iz + rh]) { extend = 0; break; }
                if (extend) rh++;
            }
            for (int a = 0; a < rw; a++)
                for (int b = 0; b < rh; b++) border[(ix + a) * h + iz + b] = 0;
            char line[160];
            snprintf(line, sizeof line, "fill %lld %d %lld %lld %d %lld magma_block replace air",
                     (long long)(bx0 + ix), floorY, (long long)(bz0 + iz),
                     (long long)(bx0 + ix + rw - 1), floorY, (long long)(bz0 + iz + rh - 1));
            monolithAdd(&borderCmds, line);
        }

    monolithEnd(&floorCmds);
    monolithEnd(&borderCmds);

    char give[512];
    giveCommand(give, sizeof give, opt->version);

    FILE *f = floorCmds.failed || borderCmds.failed ? NULL : fopen(path, "w");
    if (f) {
        fprintf(f, "seed %lld  center %lld %lld  radius %d  floor y %d  version %s\n",
                (long long)opt->seed, (long long)opt->x, (long long)opt->z, RADIUS, floorY,
                VERSION_NAMES[opt->version]);
        fprintf(f, "%d slime chunks of %d, %d blocks of floor, %lld magma border blocks\n\n",
                slimeCount, nx * nz, slimeCount * 256, borderBlocks);

        fprintf(f, "--- 0. SEED ---\n%lld\n\n", (long long)opt->seed);

        fprintf(f, "--- 1. TELEPORT ---\n/tp @p %lld %d %lld\n/gamerule %s false\n/seed\n\n",
                (long long)opt->x, afkY, (long long)opt->z,
                opt->version >= VERSION_1_21_11 ? "spawn_mobs" : "doMobSpawning");

        fprintf(f, "--- 2. VOID THE AREA (WorldEdit) ---\n//pos1 %lld,%d,%lld\n//pos2 %lld,%d,%lld\n//set air\n\n",
                (long long)((cxMin - 1) * 16), floorY, (long long)((czMin - 1) * 16),
                (long long)((cxMax + 1) * 16 + 15), voidTop, (long long)((czMax + 1) * 16 + 15));

        fprintf(f, "--- 3. SLIME CHUNK FLOOR - paste into ONE command block ---\n%s\n\n(%zu characters, %d fills)\n\n",
                floorCmds.data, floorCmds.len, floorCmds.count);

        fprintf(f, "--- 4. MAGMA BORDER - paste into ONE command block, run after 3 ---\n"
                   "%s\n\n(%zu characters, %d fills, %lld blocks)\n\n",
                borderCmds.data, borderCmds.len, borderCmds.count, borderBlocks);

        fprintf(f, "--- 5. SPAWNING SKIRT (WorldEdit) ---\n//pos1 %lld,%d,%lld\n//pos2 %lld,%d,%lld\n//replace air glass\n\n",
                (long long)((cxMin - 1) * 16), floorY, (long long)((czMin - 1) * 16),
                (long long)((cxMax + 1) * 16 + 15), floorY, (long long)((czMax + 1) * 16 + 15));

        fprintf(f, "--- 6. MEASURE - repeating command block, unconditional, always active ---\n%s\n", give);
        fclose(f);

        rep->slimeChunks = slimeCount;
        rep->scannedChunks = nx * nz;
        rep->floorFills = floorCmds.count;
        rep->borderFills = borderCmds.count;
        rep->borderBlocks = borderBlocks;
        rep->floorChars = floorCmds.len;
        rep->borderChars = borderCmds.len;
        rep->floorY = floorY;
    }

    free(slime);
    free(border);
    free(floorCmds.data);
    free(borderCmds.data);
    return f != NULL;
}
