#include <furi_hal.h>
#include <furi.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>

#define TAG "CrashTest"

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* draw_fault;
} CrashTest;

typedef enum {
    CrashTestViewSubmenu,
    CrashTestViewDrawFault,
} CrashTestView;

typedef enum {
    CrashTestSubmenuCheck,
    CrashTestSubmenuCheckMessage,
    CrashTestSubmenuAssert,
    CrashTestSubmenuAssertMessage,
    CrashTestSubmenuCrash,
    CrashTestSubmenuHalt,
    CrashTestSubmenuHeapUnderflow,
    CrashTestSubmenuHeapOverflow,
    CrashTestSubmenuBusFault,
    CrashTestSubmenuBusFaultDrawing,
    CrashTestSubmenuNullCall,
} CrashTestSubmenu;

static void crash_test_bus_fault(void) {
    // A read through a bad pointer inside a firmware function: strlen on an address that no
    // memory answers, the external memory bank, empty on the Flipper. The crash address should
    // name the instruction in strlen that faulted, not the fault handler. Both are volatile, or
    // the compiler drops the call whose result goes unused
    volatile uintptr_t nowhere = 0x60000000;
    volatile size_t length = strlen((const char*)nowhere);
    UNUSED(length);
    furi_crash("Test failed, should've crashed with \"BusFault\"");
}

static void crash_test_draw_fault_callback(Canvas* canvas, void* model) {
    // The same bus fault, while drawing: drawing runs on the GUI service's thread, whose stack
    // lies in the second memory bank, SRAM2, unlike an app thread's
    UNUSED(canvas);
    UNUSED(model);
    crash_test_bus_fault();
}

static void crash_test_null_call(void) {
    // A call through a null function pointer inside a firmware function: furi_hal_info_get calls
    // its output callback without checking it. The crash address should name that call, not the
    // fault handler
    furi_hal_info_get(NULL, '.', NULL);
    furi_crash("Test failed, should've crashed with \"MemManage\"");
}

static void crash_test_corrupt_heap_underflow(void) {
    const size_t block_size = 1000;
    const size_t underflow_size = 123;
    uint8_t* block = malloc(block_size);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow" // that's what we want!
    memset(block - underflow_size, 0xDD, underflow_size); // -V769
#pragma GCC diagnostic pop

    free(block); // should crash here (if compiled with DEBUG=1)

    // If we got here, the heap wasn't able to detect our corruption and crash
    furi_crash("Test failed, should've crashed with \"FreeRTOS Assert\" error");
}

static void crash_test_corrupt_heap_overflow(void) {
    const size_t block_size = 1000;
    const size_t overflow_size = 123;
    uint8_t* block1 = malloc(block_size);
    uint8_t* block2 = malloc(block_size);
    memset(block2, 12, 34); // simulate use to avoid optimization // -V597 // -V1086

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow" // that's what we want!
    memset(block1 + block_size, 0xDD, overflow_size); // -V769 // -V512
#pragma GCC diagnostic pop

    uint8_t* block3 = malloc(block_size);
    memset(block3, 12, 34); // simulate use to avoid optimization // -V597 // -V1086

    free(block3); // should crash here (if compiled with DEBUG=1)
    free(block2);
    free(block1);

    // If we got here, the heap wasn't able to detect our corruption and crash
    furi_crash("Test failed, should've crashed with \"FreeRTOS Assert\" error");
}

static void crash_test_submenu_callback(void* context, uint32_t index) {
    CrashTest* instance = (CrashTest*)context;

    switch(index) {
    case CrashTestSubmenuCheck:
        furi_check(false);
        break;
    case CrashTestSubmenuCheckMessage:
        furi_check(false, "Crash test: furi_check with message");
        break;
    case CrashTestSubmenuAssert:
        furi_assert(false);
        break;
    case CrashTestSubmenuAssertMessage:
        furi_assert(false, "Crash test: furi_assert with message");
        break;
    case CrashTestSubmenuCrash:
        furi_crash("Crash test: furi_crash");
        break;
    case CrashTestSubmenuHalt:
        furi_halt("Crash test: furi_halt");
        break;
    case CrashTestSubmenuHeapUnderflow:
        crash_test_corrupt_heap_underflow();
        break;
    case CrashTestSubmenuHeapOverflow:
        crash_test_corrupt_heap_overflow();
        break;
    case CrashTestSubmenuBusFault:
        crash_test_bus_fault();
        break;
    case CrashTestSubmenuBusFaultDrawing:
        view_dispatcher_switch_to_view(instance->view_dispatcher, CrashTestViewDrawFault);
        break;
    case CrashTestSubmenuNullCall:
        crash_test_null_call();
        break;
    default:
        furi_crash();
    }
}

static uint32_t crash_test_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

CrashTest* crash_test_alloc(void) {
    CrashTest* instance = malloc(sizeof(CrashTest));

    View* view = NULL;

    instance->gui = furi_record_open(RECORD_GUI);
    instance->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(
        instance->view_dispatcher, instance->gui, ViewDispatcherTypeFullscreen);

    // Menu
    instance->submenu = submenu_alloc();
    view = submenu_get_view(instance->submenu);
    view_set_previous_callback(view, crash_test_exit_callback);
    view_dispatcher_add_view(instance->view_dispatcher, CrashTestViewSubmenu, view);
    // A view whose drawing crashes
    instance->draw_fault = view_alloc();
    view_set_draw_callback(instance->draw_fault, crash_test_draw_fault_callback);
    view_dispatcher_add_view(
        instance->view_dispatcher, CrashTestViewDrawFault, instance->draw_fault);
    submenu_add_item(
        instance->submenu, "Check", CrashTestSubmenuCheck, crash_test_submenu_callback, instance);
    submenu_add_item(
        instance->submenu,
        "Check with message",
        CrashTestSubmenuCheckMessage,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu, "Assert", CrashTestSubmenuAssert, crash_test_submenu_callback, instance);
    submenu_add_item(
        instance->submenu,
        "Assert with message",
        CrashTestSubmenuAssertMessage,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu, "Crash", CrashTestSubmenuCrash, crash_test_submenu_callback, instance);
    submenu_add_item(
        instance->submenu, "Halt", CrashTestSubmenuHalt, crash_test_submenu_callback, instance);
    submenu_add_item(
        instance->submenu,
        "Heap underflow",
        CrashTestSubmenuHeapUnderflow,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu,
        "Heap overflow",
        CrashTestSubmenuHeapOverflow,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu,
        "Bus fault in firmware",
        CrashTestSubmenuBusFault,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu,
        "Bus fault while drawing",
        CrashTestSubmenuBusFaultDrawing,
        crash_test_submenu_callback,
        instance);
    submenu_add_item(
        instance->submenu,
        "Null call in firmware",
        CrashTestSubmenuNullCall,
        crash_test_submenu_callback,
        instance);

    return instance;
}

void crash_test_free(CrashTest* instance) {
    view_dispatcher_remove_view(instance->view_dispatcher, CrashTestViewDrawFault);
    view_free(instance->draw_fault);
    view_dispatcher_remove_view(instance->view_dispatcher, CrashTestViewSubmenu);
    submenu_free(instance->submenu);

    view_dispatcher_free(instance->view_dispatcher);
    furi_record_close(RECORD_GUI);

    free(instance);
}

int32_t crash_test_run(CrashTest* instance) {
    view_dispatcher_switch_to_view(instance->view_dispatcher, CrashTestViewSubmenu);
    view_dispatcher_run(instance->view_dispatcher);
    return 0;
}

int32_t crash_test_app(void* p) {
    UNUSED(p);

    CrashTest* instance = crash_test_alloc();

    int32_t ret = crash_test_run(instance);

    crash_test_free(instance);

    return ret;
}
