// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments' K3 wakeup irqchip driver
 *
 * Copyright (C) 2022 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/cpu_pm.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/soc/ti/ti_sci_protocol.h>

#define TISCI_HAS_IRQ		(pcs->flags & PCS_FEAT_IRQ)

/**
 * struct ti_sci_wkup_irq_domain - Structure representing a TISCI based
 *				   Interrupt Router IRQ domain.
 * @sci:	Pointer to TISCI handle
 */
struct ti_sci_wkup_irq_domain {
#define PCS_QUIRK_SHARED_IRQ	(1 << 2)
#define PCS_FEAT_IRQ		(1 << 1)
	const struct ti_sci_handle *sci;
	struct irq_domain *irq_domain;
	struct notifier_block nb;
	struct device *dev;
	struct delayed_work wq_wkup;
	int irq;
	unsigned flags;
	u32 type;
};

static struct irq_chip ti_sci_wkup_irq_chip = {
	.name = "ti-sci-wkup",
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static int ti_sci_wkup_irq_domain_map(struct irq_domain *d, unsigned int irq,
				      irq_hw_number_t hw)
{
	struct ti_sci_wkup_irq_domain *intw = d->host_data;

	irq_set_chip_and_handler(irq, &ti_sci_wkup_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, intw);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops ti_sci_wkup_irq_domain_ops = {
	.map = ti_sci_wkup_irq_domain_map,
	.xlate = irq_domain_xlate_onecell,
};

static int ti_sci_wkup_handle_wake_reason(struct ti_sci_wkup_irq_domain *intw)
{
	const struct ti_sci_pm_ops *pmops = &intw->sci->ops.pm_ops;
	u64 timestamp;
	u32 wkup_source;
	int virq, ret;

	ret = pmops->lpm_wake_reason(intw->sci, &wkup_source, &timestamp);
	if (ret)
		return ret;

	virq = irq_find_mapping(intw->irq_domain, wkup_source);
	if (virq) {
		generic_handle_irq(virq);
		dev_info(intw->dev, "Handled wake reason 0x%x virq %d, timestamp %llu\n",
			 wkup_source, virq, timestamp);
		return 0;
	}

	dev_info(intw->dev, "Not handled wake reason 0x%x, timestamp %llu\n",
		 wkup_source, timestamp);
	return 0;
}

static void ti_sci_wkup_work(struct work_struct *work)
{
	struct ti_sci_wkup_irq_domain *intw =
		container_of(to_delayed_work(work),
			     struct ti_sci_wkup_irq_domain, wq_wkup);

	ti_sci_wkup_handle_wake_reason(intw);
}

static int ti_sci_wkup_notifier(struct notifier_block *nb,
				unsigned long cmd, void *v)
{
	struct ti_sci_wkup_irq_domain *intw;

	printk("mydbg: %s", __func__);
	intw = container_of(nb, struct ti_sci_wkup_irq_domain, nb);

	/* The notifier runs with timekeeping suspended.*/
	if (cmd == CPU_CLUSTER_PM_EXIT)
		ti_sci_wkup_handle_wake_reason(intw);
	return NOTIFY_OK;
}

static irqreturn_t intw_irq_chain_handler(int irq, void *d)
{
	printk("%s: TODO....", __func__);
	return IRQ_HANDLED;
}

static int ti_sci_wkup_irq_domain_probe(struct platform_device *pdev)
{
	struct ti_sci_wkup_irq_domain *intw;
	struct device *dev = &pdev->dev;
	struct device_node *dev_node_intw = pdev->dev.of_node;
	int res;

	printk("mydbg: %s", __func__);

	intw = devm_kzalloc(dev, sizeof(*intw), GFP_KERNEL);
	if (!intw)
		return -ENOMEM;

	intw->dev = dev;
	platform_set_drvdata(pdev, intw);

	intw->sci = devm_ti_sci_get_by_phandle(dev, "ti,sci");
	if (IS_ERR(intw->sci))
		return dev_err_probe(dev, PTR_ERR(intw->sci),
				     "ti,sci read fail\n");

	intw->irq = irq_of_parse_and_map(dev_node_intw, 0);
	printk("mydbg: %s .. int irq %d", __func__, intw->irq);
	if (intw->irq)
	{
		printk("mydbg: %s .. int irq %d", __func__, intw->irq);
		intw->flags |= PCS_FEAT_IRQ;
	}

	intw->irq_domain = irq_domain_add_tree(dev_of_node(dev),
					       &ti_sci_wkup_irq_domain_ops,
					       intw);
	if (!intw->irq_domain)
		return -ENOMEM;

	intw->nb.notifier_call = ti_sci_wkup_notifier;
	cpu_pm_register_notifier(&intw->nb);


	const char *name = "tiscirq";
		res = request_irq(intw->irq, intw_irq_chain_handler,
				  IRQF_SHARED | IRQF_NO_SUSPEND |
				  IRQF_NO_THREAD,
				  name, intw);
		if (res) {
		printk("mydbg: %s .. res =  %d", __func__, res);
			intw->irq = -1;
			return res;
		}

	INIT_DELAYED_WORK(&intw->wq_wkup, ti_sci_wkup_work);
	dev_info(dev, "Wakeup interrupt domain created\n");

	return 0;
}

static int ti_sci_wkup_irq_domain_remove(struct platform_device *pdev)
{
	struct ti_sci_wkup_irq_domain *intw = platform_get_drvdata(pdev);

	cpu_pm_unregister_notifier(&intw->nb);
	irq_domain_remove(intw->irq_domain);

	return 0;
}

static const struct of_device_id ti_sci_wkup_irq_domain_of_match[] = {
	{ .compatible = "ti,sci-wkup", },
	{ },
};
MODULE_DEVICE_TABLE(of, ti_sci_wkup_irq_domain_of_match);

static struct platform_driver ti_sci_wkup_irq_domain_driver = {
	.probe = ti_sci_wkup_irq_domain_probe,
	.remove = ti_sci_wkup_irq_domain_remove,
	.driver = {
		.name = "ti-sci-wkup",
		.of_match_table = ti_sci_wkup_irq_domain_of_match,
	},
};
module_platform_driver(ti_sci_wkup_irq_domain_driver);
