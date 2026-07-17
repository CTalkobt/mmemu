#include "../main/lua_event_registry.h"
#include "test_harness.h"

TEST_CASE(lua_event_register_cycle) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(10000, "my_handler");
    ASSERT_EQ(registry.cycleEventCount(), 1);
}

TEST_CASE(lua_event_register_interrupt) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "on_irq");
    ASSERT_EQ(registry.interruptEventCount(), 1);
}

TEST_CASE(lua_event_get_handler) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("NMI", "on_nmi");

    const auto* handler = registry.getInterruptHandler("NMI");
    ASSERT(handler != nullptr);
    ASSERT_EQ(*handler, "on_nmi");
}

TEST_CASE(lua_event_nonexistent_handler) {
    LuaEventRegistry registry;
    const auto* handler = registry.getInterruptHandler("IRQ");
    ASSERT(handler == nullptr);
}

TEST_CASE(lua_event_cycle_fires_correctly) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");

    // Before first firing
    auto ready = registry.getReadyCycleHandlers(500);
    ASSERT_EQ(ready.size(), 0);

    // At first firing
    ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler1");

    // After first firing
    ready = registry.getReadyCycleHandlers(1500);
    ASSERT_EQ(ready.size(), 0);

    // At second firing
    ready = registry.getReadyCycleHandlers(2000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler1");
}

TEST_CASE(lua_event_multiple_cycles) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    registry.registerCycleEvent(2000, "handler2");

    auto ready = registry.getReadyCycleHandlers(2000);
    ASSERT_EQ(ready.size(), 2);
    ASSERT(std::find(ready.begin(), ready.end(), "handler1") != ready.end());
    ASSERT(std::find(ready.begin(), ready.end(), "handler2") != ready.end());
}

TEST_CASE(lua_event_single_fire_at_interval) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    registry.registerCycleEvent(2000, "handler2");

    // At 1000: handler1 fires
    auto ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler1");

    // At 1500: neither handler fires (handler1 next at 2000, handler2 next at 2000)
    ready = registry.getReadyCycleHandlers(1500);
    ASSERT_EQ(ready.size(), 0);
}

TEST_CASE(lua_event_replace_interrupt) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "old_handler");
    registry.registerInterruptEvent("IRQ", "new_handler");

    const auto* handler = registry.getInterruptHandler("IRQ");
    ASSERT_EQ(*handler, "new_handler");
}

TEST_CASE(lua_event_multiple_interrupts) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "on_irq");
    registry.registerInterruptEvent("NMI", "on_nmi");
    registry.registerInterruptEvent("BRK", "on_brk");

    ASSERT_EQ(*registry.getInterruptHandler("IRQ"), "on_irq");
    ASSERT_EQ(*registry.getInterruptHandler("NMI"), "on_nmi");
    ASSERT_EQ(*registry.getInterruptHandler("BRK"), "on_brk");
    ASSERT_EQ(registry.interruptEventCount(), 3);
}

TEST_CASE(lua_event_clear_all) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    registry.registerInterruptEvent("IRQ", "on_irq");
    ASSERT_EQ(registry.cycleEventCount(), 1);
    ASSERT_EQ(registry.interruptEventCount(), 1);

    registry.clear();
    ASSERT_EQ(registry.cycleEventCount(), 0);
    ASSERT_EQ(registry.interruptEventCount(), 0);
}

TEST_CASE(lua_event_unregister_cycle) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    registry.registerCycleEvent(2000, "handler2");
    ASSERT_EQ(registry.cycleEventCount(), 2);

    registry.unregisterCycleEvent(0);
    ASSERT_EQ(registry.cycleEventCount(), 1);
}

TEST_CASE(lua_event_unregister_interrupt) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "on_irq");
    ASSERT_EQ(registry.interruptEventCount(), 1);

    registry.unregisterInterruptEvent("IRQ");
    ASSERT_EQ(registry.interruptEventCount(), 0);
}

TEST_CASE(lua_event_invalid_cycle_interval) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(0, "handler");
    ASSERT_EQ(registry.cycleEventCount(), 0);
}

TEST_CASE(lua_event_invalid_interrupt_type) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("INVALID", "handler");
    ASSERT_EQ(registry.interruptEventCount(), 0);
}

TEST_CASE(lua_event_all_fire_at_same_cycle) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    registry.registerCycleEvent(1000, "handler2");
    registry.registerCycleEvent(1000, "handler3");

    auto ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 3);
    ASSERT(std::find(ready.begin(), ready.end(), "handler1") != ready.end());
    ASSERT(std::find(ready.begin(), ready.end(), "handler2") != ready.end());
    ASSERT(std::find(ready.begin(), ready.end(), "handler3") != ready.end());
}

TEST_CASE(lua_event_handler_order_preserved) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(500, "first");
    registry.registerCycleEvent(500, "second");
    registry.registerCycleEvent(500, "third");

    auto ready = registry.getReadyCycleHandlers(500);
    ASSERT_EQ(ready.size(), 3);
    // Handlers should be returned in registration order
    ASSERT_EQ(ready[0], "first");
    ASSERT_EQ(ready[1], "second");
    ASSERT_EQ(ready[2], "third");
}

TEST_CASE(lua_event_duplicate_call_same_cycle) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");

    auto ready1 = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready1.size(), 1);
    ASSERT_EQ(ready1[0], "handler1");

    // Second call at same cycle should return no handlers
    // (nextFire was updated to 2000 by first call)
    auto ready2 = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready2.size(), 0);
}

TEST_CASE(lua_event_same_function_different_intervals) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler");
    registry.registerCycleEvent(2000, "handler");

    // At 1000: only first event fires
    auto ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler");

    // At 2000: both events fire (1000-interval fires again, 2000-interval fires first time)
    ready = registry.getReadyCycleHandlers(2000);
    ASSERT_EQ(ready.size(), 2);
    ASSERT_EQ(ready[0], "handler");
    ASSERT_EQ(ready[1], "handler");
}

TEST_CASE(lua_event_reregister_after_unregister) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler");
    ASSERT_EQ(registry.cycleEventCount(), 1);

    registry.unregisterCycleEvent(0);
    ASSERT_EQ(registry.cycleEventCount(), 0);

    // Re-register the same handler
    registry.registerCycleEvent(1000, "handler");
    ASSERT_EQ(registry.cycleEventCount(), 1);

    auto ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler");
}

TEST_CASE(lua_event_unregister_invalid_index) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler1");
    ASSERT_EQ(registry.cycleEventCount(), 1);

    // Unregister with out-of-bounds index should not crash
    registry.unregisterCycleEvent(5);
    ASSERT_EQ(registry.cycleEventCount(), 1);

    // Unregister with size() index should not crash
    registry.unregisterCycleEvent(1);
    ASSERT_EQ(registry.cycleEventCount(), 1);
}

TEST_CASE(lua_event_large_cycle_numbers) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(1000, "handler");

    // Test with very large cycle number
    auto ready = registry.getReadyCycleHandlers(0xFFFFFFFFFFFF0000ULL);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler");

    // Next firing should be at current + interval
    ready = registry.getReadyCycleHandlers(0xFFFFFFFFFFFF0000ULL);
    ASSERT_EQ(ready.size(), 0);
}

TEST_CASE(lua_event_interrupt_handler_true_replacement) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "handler1");
    ASSERT_EQ(registry.interruptEventCount(), 1);
    ASSERT_EQ(*registry.getInterruptHandler("IRQ"), "handler1");

    // Replace with new handler
    registry.registerInterruptEvent("IRQ", "handler2");
    ASSERT_EQ(registry.interruptEventCount(), 1);  // Still just 1
    ASSERT_EQ(*registry.getInterruptHandler("IRQ"), "handler2");
}

TEST_CASE(lua_event_unregister_nonexistent_interrupt) {
    LuaEventRegistry registry;
    registry.registerInterruptEvent("IRQ", "handler");
    ASSERT_EQ(registry.interruptEventCount(), 1);

    // Unregister nonexistent type (should not affect existing)
    registry.unregisterInterruptEvent("NMI");
    ASSERT_EQ(registry.interruptEventCount(), 1);

    // IRQ should still be there
    ASSERT(registry.getInterruptHandler("IRQ") != nullptr);
}

TEST_CASE(lua_event_next_fire_calculation) {
    LuaEventRegistry registry;
    registry.registerCycleEvent(3, "handler");

    // At cycle 3: fires, nextFire becomes 3+3=6
    auto ready = registry.getReadyCycleHandlers(3);
    ASSERT_EQ(ready.size(), 1);

    // At cycle 4,5: no fire
    ready = registry.getReadyCycleHandlers(4);
    ASSERT_EQ(ready.size(), 0);

    ready = registry.getReadyCycleHandlers(5);
    ASSERT_EQ(ready.size(), 0);

    // At cycle 6: fires, nextFire becomes 6+3=9
    ready = registry.getReadyCycleHandlers(6);
    ASSERT_EQ(ready.size(), 1);

    // At cycle 9: fires
    ready = registry.getReadyCycleHandlers(9);
    ASSERT_EQ(ready.size(), 1);
}
