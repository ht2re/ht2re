// ht2editor - CLI entry point (docs/editor.md).
#include "EditorApp.h"
#include "util/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ht2;

static bool argIs(const char* a, const char* name) { return strcmp(a, name) == 0; }

int main(int argc, char** argv) {
    EditorOptions o;
    for (int i = 1; i < argc; ++i) {
        if (argIs(argv[i], "--data") && i + 1 < argc) o.dataDir = argv[++i];
        else if (argIs(argv[i], "--map") && i + 1 < argc) o.mapTile = argv[++i];
        else if (argIs(argv[i], "--vehicle") && i + 1 < argc) o.vehicle = argv[++i];
        else if (argIs(argv[i], "--overlay") && i + 1 < argc) o.overlay = argv[++i];
        else if (argIs(argv[i], "--shot") && i + 1 < argc) o.shot = argv[++i];
        else if (argIs(argv[i], "--select") && i + 1 < argc) o.select = argv[++i];
        else if (argIs(argv[i], "--script") && i + 1 < argc) o.script = argv[++i];
        else if (argIs(argv[i], "--cmd") && i + 1 < argc) o.cmds.push_back(argv[++i]);
        else if (argIs(argv[i], "--view") && i + 1 < argc) o.view = argv[++i];
        else if (argIs(argv[i], "--list")) o.list = true;
        else if (argIs(argv[i], "--blank")) o.blank = true;
        else if (argIs(argv[i], "--no-textures")) o.textures = false;
        else if (argIs(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &o.w, &o.h) != 2) { LOG("bad --size"); return 1; }
        }
        else if (argIs(argv[i], "--angle") && i + 1 < argc) {
            float y = 0, p = 0;
            int n = sscanf(argv[++i], "%f,%f", &y, &p);
            if (n < 1) { LOG("bad --angle"); return 1; }
            o.angle[0] = y;
            if (n >= 2) o.angle[1] = p;
            o.angleGiven = true;
        }
        else if (argIs(argv[i], "--cam") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f,%f,%f,%f,%f", &o.cam[0], &o.cam[1], &o.cam[2],
                       &o.cam[3], &o.cam[4]) != 5) { LOG("bad --cam"); return 1; }
            o.camGiven = true;
        }
        else if (argIs(argv[i], "--help") || argIs(argv[i], "-h")) {
            EditorApp a;
            EditorOptions d;
            a.run(d);     // prints usage (no --map/--vehicle given)
            return 0;
        }
        else {
            LOG("ht2editor: unknown option '%s'", argv[i]);
            EditorApp a;
            a.run(EditorOptions{});
            return 1;
        }
    }
    EditorApp app;
    return app.run(o);
}
