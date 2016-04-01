/*
 *  linux/drivers/clocksource/timer-sp.c
 *
 *  Copyright (C) 1999 - 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#include <clocksource/timer-sp804.h>

#include "timer-sp.h"

static long __init sp804_get_clock_rate(struct clk *clk)
{
	long rate;
	int err;

	err = clk_prepare(clk);
	if (err) {
		pr_err("sp804: clock failed to prepare: %d\n", err);
		clk_put(clk);
		return err;
	}

	err = clk_enable(clk);
	if (err) {
		pr_err("sp804: clock failed to enable: %d\n", err);
		clk_unprepare(clk);
		clk_put(clk);
		return err;
	}

	rate = clk_get_rate(clk);
	if (rate < 0) {
		pr_err("sp804: clock failed to get rate: %ld\n", rate);
		clk_disable(clk);
		clk_unprepare(clk);
		clk_put(clk);
	}

	return rate;
}

static void __iomem *sched_clock_base;

static u64 notrace sp804_read(void)
{
	return ~readl_relaxed(sched_clock_base + TIMER_VALUE);
}

void __init sp804_timer_disable(void __iomem *base)
{
	writel(0, base + TIMER_CTRL);
}

void __init __sp804_clocksource_and_sched_clock_init(struct timer_sp804 *sp804,
						     bool use_sched_clock)
{
	long rate;
	u32 config = TIMER_CTRL_ENABLE | TIMER_CTRL_PERIODIC;
	struct clk *clk = sp804->clocksource_clk;

	if (!clk) {
		clk = clk_get_sys("sp804", sp804->name);
		if (IS_ERR(clk)) {
			pr_err("sp804: clock not found: %d\n",
			       (int)PTR_ERR(clk));
			return;
		}
	}

	rate = sp804_get_clock_rate(clk);

	if (rate < 0)
		return;

	if (sp804->width == 32)
		config |= TIMER_CTRL_32BIT;

	/* setup timer 0 as free-running clocksource */
	writel(0, sp804->clocksource_base + TIMER_CTRL);
	writel(0xffffffff, sp804->clocksource_base + TIMER_LOAD);
	writel(0xffffffff, sp804->clocksource_base + TIMER_VALUE);
	writel(config, sp804->clocksource_base + TIMER_CTRL);

	clocksource_mmio_init(sp804->clocksource_base + TIMER_VALUE,
			      sp804->name, rate, 200, sp804->width,
			      clocksource_mmio_readl_down);

	if (use_sched_clock) {
		sched_clock_base = sp804->clocksource_base;
		sched_clock_register(sp804_read, sp804->width, rate);
	}
}


static void __iomem *clkevt_base;
static unsigned long clkevt_reload;

/*
 * IRQ handler for the timer
 */
static irqreturn_t sp804_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	/* clear the interrupt */
	writel(1, clkevt_base + TIMER_INTCLR);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static inline void timer_shutdown(struct clock_event_device *evt)
{
	writel(0, clkevt_base + TIMER_CTRL);
}

static int sp804_shutdown(struct clock_event_device *evt)
{
	timer_shutdown(evt);
	return 0;
}

static int sp804_set_periodic(struct clock_event_device *evt)
{
	unsigned long ctrl = TIMER_CTRL_32BIT | TIMER_CTRL_IE |
			     TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE;

	timer_shutdown(evt);
	writel(clkevt_reload, clkevt_base + TIMER_LOAD);
	writel(ctrl, clkevt_base + TIMER_CTRL);
	return 0;
}

static int sp804_set_next_event(unsigned long next,
	struct clock_event_device *evt)
{
	unsigned long ctrl = TIMER_CTRL_32BIT | TIMER_CTRL_IE |
			     TIMER_CTRL_ONESHOT | TIMER_CTRL_ENABLE;

	writel(next, clkevt_base + TIMER_LOAD);
	writel(ctrl, clkevt_base + TIMER_CTRL);

	return 0;
}

static struct clock_event_device sp804_clockevent = {
	.features		= CLOCK_EVT_FEAT_PERIODIC |
				  CLOCK_EVT_FEAT_ONESHOT |
				  CLOCK_EVT_FEAT_DYNIRQ,
	.set_state_shutdown	= sp804_shutdown,
	.set_state_periodic	= sp804_set_periodic,
	.set_state_oneshot	= sp804_shutdown,
	.tick_resume		= sp804_shutdown,
	.set_next_event		= sp804_set_next_event,
	.rating			= 300,
};

static struct irqaction sp804_timer_irq = {
	.name		= "timer",
	.flags		= IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= sp804_timer_interrupt,
	.dev_id		= &sp804_clockevent,
};

void __init __sp804_clockevents_init(struct timer_sp804 *sp804)
{
	struct clock_event_device *evt = &sp804_clockevent;
	long rate;
	struct clk *clk = sp804->clockevent_clk;

	if (!clk)
		clk = clk_get_sys("sp804", sp804->name);
	if (IS_ERR(clk)) {
		pr_err("sp804: %s clock not found: %d\n", sp804->name,
			(int)PTR_ERR(clk));
		return;
	}

	rate = sp804_get_clock_rate(clk);
	if (rate < 0)
		return;

	clkevt_base = sp804->clockevent_base;
	clkevt_reload = DIV_ROUND_CLOSEST(rate, HZ);
	evt->name = sp804->name;
	evt->irq = sp804->irq;
	evt->cpumask = cpu_possible_mask;

	writel(0, sp804->clockevent_base + TIMER_CTRL);

	setup_irq(sp804->irq, &sp804_timer_irq);
	clockevents_config_and_register(evt, rate, 0xf,
					GENMASK(sp804->width-1, 0));
}

static void __init sp804_of_init(struct device_node *np)
{
	static bool initialized = false;
	struct timer_sp804 sp804;
	void __iomem *base;
	u32 irq_num = 0;
	struct clk *clk1, *clk2;
	u32 width = 32;

	base = of_iomap(np, 0);
	if (WARN_ON(!base))
		return;

	/* Ensure timers are disabled */
	writel(0, base + TIMER_CTRL);
	writel(0, base + TIMER_2_BASE + TIMER_CTRL);

	if (initialized || !of_device_is_available(np))
		goto err;

	clk1 = of_clk_get(np, 0);
	if (IS_ERR(clk1))
		clk1 = NULL;

	/* Get the 2nd clock if the timer has 3 timer clocks */
	if (of_count_phandle_with_args(np, "clocks", "#clock-cells") == 3) {
		clk2 = of_clk_get(np, 1);
		if (IS_ERR(clk2)) {
			pr_err("sp804: %s clock not found: %d\n", np->name,
				(int)PTR_ERR(clk2));
			clk2 = NULL;
		}
	} else
		clk2 = clk1;

	sp804.irq = irq_of_parse_and_map(np, 0);
	if (sp804.irq <= 0)
		goto err;

	/* OX810SE Uses a special 24bit width */
	if (of_device_is_compatible(np, "oxsemi,ox810se-rps-timer"))
		width = 24;

	of_property_read_u32(np, "arm,sp804-has-irq", &irq_num);
	if (irq_num == 2) {
		sp804.clockevent_base = base + TIMER_2_BASE;
		sp804.clocksource_base = base;
		sp804.clockevent_clk = clk2;
		sp804.clocksource_clk = clk1;
	} else {
		sp804.clockevent_base = base;
		sp804.clocksource_base = base + TIMER_2_BASE;
		sp804.clockevent_clk = clk1;
		sp804.clocksource_clk = clk2;
	}

	sp804.name = of_get_property(np, "compatible", NULL);
	sp804.width = width;

	__sp804_clockevents_init(&sp804);
	__sp804_clocksource_and_sched_clock_init(&sp804, true);

	initialized = true;

	return;
err:
	iounmap(base);
}
CLOCKSOURCE_OF_DECLARE(sp804, "arm,sp804", sp804_of_init);
CLOCKSOURCE_OF_DECLARE(ox810se, "oxsemi,ox810se-rps-timer", sp804_of_init);

static void __init integrator_cp_of_init(struct device_node *np)
{
	static int init_count = 0;
	struct timer_sp804 sp804;
	void __iomem *base;
	struct clk *clk;

	base = of_iomap(np, 0);
	if (WARN_ON(!base))
		return;
	clk = of_clk_get(np, 0);
	if (WARN_ON(IS_ERR(clk)))
		return;

	/* Ensure timer is disabled */
	writel(0, base + TIMER_CTRL);

	if (init_count == 2 || !of_device_is_available(np))
		goto err;

	sp804.name = of_get_property(np, "compatible", NULL);
	sp804.width = 32;

	if (!init_count) {
		sp804.clocksource_base = base;
		sp804.clocksource_clk = clk;

		__sp804_clocksource_and_sched_clock_init(&sp804, false);
	} else {
		sp804.clockevent_base = base;
		sp804.clockevent_clk = clk;
		sp804.irq = irq_of_parse_and_map(np, 0);
		if (sp804.irq <= 0)
			goto err;

		__sp804_clockevents_init(&sp804);
	}

	init_count++;
	return;
err:
	iounmap(base);
}
CLOCKSOURCE_OF_DECLARE(intcp, "arm,integrator-cp-timer", integrator_cp_of_init);
