#include "BootUtils.h"
#include "ACTAccountInfo.h"
#include "DrawUtils.h"
#include "MenuUtils.h"
#include "logger.h"
#include <codecvt>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <locale>
#include <malloc.h>
#include <memory>
#include <mocha/mocha.h>
#include <nn/act.h>
#include <nn/cmpt/cmpt.h>
#include <padscore/kpad.h>
#include <sndcore2/core.h>
#include <string>
#include <sysapp/launch.h>
#include <sysapp/title.h>
#include <vector>

void handleAccountSelection();

void bootWiiUMenu() {
    nn::act::Initialize();
    nn::act::SlotNo slot        = nn::act::GetSlotNo();
    nn::act::SlotNo defaultSlot = nn::act::GetDefaultAccount();
    nn::act::Finalize();

    if (defaultSlot) { //normal menu boot
        SYSLaunchMenu();
    } else { //show mii select
        _SYSLaunchMenuWithCheckingAccount(slot);
    }
}

void bootHomebrewLauncher() {
    handleAccountSelection();

    uint64_t titleId = _SYSGetSystemApplicationTitleId(SYSTEM_APP_ID_MII_MAKER);
    _SYSLaunchTitleWithStdArgsInNoSplash(titleId, nullptr);
}

void handleAccountSelection() {
    nn::act::Initialize();
    nn::act::SlotNo defaultSlot = nn::act::GetDefaultAccount();

    if (!defaultSlot) { // No default account is set.
        std::vector<std::shared_ptr<AccountInfo>> accountInfoList;
        for (int32_t i = 0; i < 13; i++) {
            if (!nn::act::IsSlotOccupied(i)) {
                continue;
            }
            char16_t nameOut[nn::act::MiiNameSize];
            std::shared_ptr<AccountInfo> accountInfo = std::make_shared<AccountInfo>();
            accountInfo->slot                        = i;
            auto result                              = nn::act::GetMiiNameEx(reinterpret_cast<int16_t *>(nameOut), i);
            if (result.IsSuccess()) {
                std::u16string source;
                std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
                accountInfo->name = convert.to_bytes((char16_t *) nameOut);
            } else {
                accountInfo->name = "[UNKNOWN]";
            }
            accountInfo->isNetworkAccount = nn::act::IsNetworkAccountEx(i);
            if (accountInfo->isNetworkAccount) {
                nn::act::GetAccountIdEx(accountInfo->accountId, i);
            }

            uint32_t imageSize = 0;
            result             = nn::act::GetMiiImageEx(&imageSize, accountInfo->miiImageBuffer, sizeof(accountInfo->miiImageBuffer), 0, i);
            if (result.IsSuccess()) {
                accountInfo->miiImageSize = imageSize;
            }
            accountInfoList.push_back(accountInfo);
        }

        if (accountInfoList.size() > 0) {
            if (!AXIsInit()) {
                AXInit();
            }
            auto slot = handleAccountSelectScreen(accountInfoList);

            DEBUG_FUNCTION_LINE("Load slot %d", slot);
            nn::act::LoadConsoleAccount(slot, 0, nullptr, false);
        }
    }
    nn::act::Finalize();
}

// Exported by nn_cmpt.rpl but not declared in wut's cmpt.h; the Wii U Menu
// calls this before launching vWii titles to signal GamePad support.
extern "C" int32_t CMPTAcctSetDrcCtrlEnabled(int32_t enabled);

// Diagnostic stage markers for the padless vWii launch: fill the whole TV
// screen with a solid color so the last stage reached stays visible even if
// a later call blocks forever. Draws into both flip buffers so the color
// survives until the next marker.
static void showStageColor(uint32_t color) {
    static void *screenBuffer = nullptr;
    if (!screenBuffer) {
        screenBuffer = DrawUtils::InitOSScreen();
        if (!screenBuffer) {
            return;
        }
        uint32_t tvSize  = OSScreenGetBufferSizeEx(SCREEN_TV);
        uint32_t drcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);
        DrawUtils::initBuffers(screenBuffer, tvSize, (void *) ((uint32_t) screenBuffer + tvSize), drcSize);
    }
    for (int i = 0; i < 2; i++) {
        DrawUtils::beginDraw();
        DrawUtils::clear(Color(color));
        DrawUtils::endDraw();
    }
}

// Prepare the compat (vWii) subsystem so a launch works with the GamePad
// powered off. On autoboot this runs much earlier than a manual boot-menu
// selection: CMPTAcct* calls made before nn_cmpt is up can get lost, so
// re-assert them on every poll pass and wait up to 30 s for a valid screen
// state, see AutobootModule#67. Returns false if it never became ready.
static bool prepareVWiiLaunch() {
    showStageColor(0x0000FFFF); // BLUE: entered prepareVWiiLaunch
    KPADInit();
    showStageColor(0xFF00FFFF); // MAGENTA: KPADInit returned
    CMPTAcctSetDrcCtrlEnabled(0);
    showStageColor(0x00FFFFFF); // CYAN: first DrcCtrlEnabled returned
    CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_TV);
    for (int i = 0; i < 300; i++) {
        // YELLOW blink (1 s cycle): poll loop alive; solid = a call blocks
        showStageColor((i / 10) % 2 ? 0xFFFF00FF : 0x806000FF);
        CMPTAcctSetDrcCtrlEnabled(0);
        CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_TV);
        if (CMPTCheckScreenState() >= 0) {
            showStageColor(0x00FF00FF); // GREEN: subsystem ready
            return true;
        }
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    DEBUG_FUNCTION_LINE_ERR("Compat subsystem not ready after 30s");
    showStageColor(0xFF0000FF); // RED: timeout, falling back to Wii U Menu
    OSSleepTicks(OSMillisecondsToTicks(2000));
    return false;
}

static void launchvWiiTitle(uint64_t titleId) {
    if (!prepareVWiiLaunch()) {
        // Compat subsystem never became ready; boot the Wii U Menu instead of
        // hanging on a black screen — vWii stays reachable from there.
        bootWiiUMenu();
        return;
    }

    uint32_t dataSize = 0;
    CMPTGetDataSize(&dataSize);

    void *dataBuffer = memalign(0x40, dataSize);

    showStageColor(0xFFFFFFFF); // WHITE: about to call CMPTLaunch*
    if (titleId == 0) {
        CMPTLaunchMenu(dataBuffer, dataSize);
    } else {
        CMPTLaunchTitle(dataBuffer, dataSize, titleId);
    }
    // GREY: CMPTLaunch* returned — transition to vWii happens after this
    showStageColor(0x808080FF);

    free(dataBuffer);
}

void bootvWiiMenu() {
    launchvWiiTitle(0);
}

uint64_t getVWiiHBLTitleId() {
    // fall back to booting the vWii system menu if anything fails
    uint64_t titleId = 0;

    FSAInit();
    auto client = FSAAddClient(nullptr);
    if (client > 0) {
        if (Mocha_UnlockFSClientEx(client) == MOCHA_RESULT_SUCCESS) {
            // mount the slccmpt
            if (FSAMount(client, "/dev/slccmpt01", "/vol/storage_abm_slccmpt01", FSA_MOUNT_FLAG_GLOBAL_MOUNT, nullptr, 0) >= 0) {
                FSStat stat;

                // test if the OHBC or HBC is installed
                if (FSAGetStat(client, "/vol/storage_abm_slccmpt01/title/00010001/4f484243/content/00000000.app", &stat) >= 0) {
                    titleId = 0x000100014F484243L; // 'OHBC'
                } else if (FSAGetStat(client, "/vol/storage_abm_slccmpt01/title/00010001/4c554c5a/content/00000000.app", &stat) >= 0) {
                    titleId = 0x000100014C554C5AL; // 'LULZ'
                } else {
                    DEBUG_FUNCTION_LINE("Cannot find HBC");
                }
                FSAUnmount(client, "/vol/storage_abm_slccmpt01", FSA_UNMOUNT_FLAG_FORCE);
            } else {
                DEBUG_FUNCTION_LINE_ERR("Failed to mount slccmpt01");
            }
        } else {
            DEBUG_FUNCTION_LINE_ERR("Failed to unlock FSClient");
        }
        FSADelClient(client);
    } else {
        DEBUG_FUNCTION_LINE_ERR("Failed to add FSAClient");
    }
    return titleId;
}

void bootHomebrewChannel() {
    // Prepare the compat subsystem BEFORE the slccmpt title lookup: the
    // lookup and the launch both race against subsystem readiness on autoboot.
    if (!prepareVWiiLaunch()) {
        bootWiiUMenu();
        return;
    }
    uint64_t titleId = getVWiiHBLTitleId();
    if (titleId == 0) {
        // HBC lookup failed; try the standard vWii HBC title id directly
        // instead of falling back to the System Menu.
        titleId = 0x000100014C554C5AL; // 'LULZ'
    }
    DEBUG_FUNCTION_LINE("Launching vWii title %016llx", titleId);
    launchvWiiTitle(titleId);
}
