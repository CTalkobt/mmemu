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

    auto ready = registry.getReadyCycleHandlers(1500);
    ASSERT_EQ(ready.size(), 0);

    ready = registry.getReadyCycleHandlers(1000);
    ASSERT_EQ(ready.size(), 1);
    ASSERT_EQ(ready[0], "handler1");
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
