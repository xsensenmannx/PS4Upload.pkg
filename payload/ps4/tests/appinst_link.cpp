/*
 * P1-Spike, zweite Frage: reicht sceAppInstUtilAppInstallPkg als Fallback,
 * wenn die BGFT-Header (orbis/Bgft.h) nicht eingebunden werden?
 *
 * Diese Datei kennt nur <stdint.h> und eigene Prototypen; uebersetzt wird sie
 * deshalb ohne jeden OpenOrbis-Include-Pfad. Nur ein erfolgreicher Link gegen
 * libSceAppInstUtil.so beantwortet die Frage. Ausgefuehrt wird das ELF nicht.
 */

#include <stdint.h>

extern "C" int32_t sceAppInstUtilInitialize(void);
extern "C" int32_t sceAppInstUtilAppInstallPkg(const char *pkg_path, void *reserved);
extern "C" int32_t sceAppInstUtilGetInstallProgress(const char *content_id,
                                                    uint32_t *percent);

int main(int argc, char **argv)
{
    uint32_t percent = 0;

    (void)sceAppInstUtilInitialize();
    (void)sceAppInstUtilAppInstallPkg(
        argc > 1 ? argv[1] : "/data/ps4upload/spike.pkg", (void *)0);
    (void)sceAppInstUtilGetInstallProgress("", &percent);

    return (int)percent;
}
