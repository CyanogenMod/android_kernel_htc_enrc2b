/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/mii.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/usb.h>
#include <linux/usb/usbnet.h>
#include <linux/msm_rmnet.h>

//++SSD_RIL:20120731: For tx/rx enable_hlt/disable_hlt
#include <asm/mach-types.h>
#include <mach/board_htc.h>
//--SSD_RIL

#include "rmnet_usb_ctrl.h"

//++SSD_RIL:20121017: get -71 but already register rmnet netdev
extern int already_register_rmNET;
//--SSD_RIL:20121017: get -71 but already register rmnet netdev

#define RMNET_DATA_LEN			2000
#define HEADROOM_FOR_QOS		8

static int	data_msg_dbg_mask;

enum {
	DEBUG_MASK_LVL0 = 1U << 0,
	DEBUG_MASK_LVL1 = 1U << 1,
	DEBUG_MASK_LVL2 = 1U << 2,
};

#define DBG(m, x...) do { \
		if (data_msg_dbg_mask & m) \
			pr_info(x); \
} while (0)

/*echo dbg_mask > /sys/class/net/rmnet_usbx/dbg_mask*/
static ssize_t dbg_mask_store(struct device *d,
		struct device_attribute *attr,
		const char *buf, size_t n)
{
	unsigned int		dbg_mask;
	struct net_device	*dev = to_net_dev(d);
	struct usbnet		*unet = netdev_priv(dev);

	if (!dev)
		return -ENODEV;

	sscanf(buf, "%u", &dbg_mask);
	/*enable dbg msgs for data driver*/
	data_msg_dbg_mask = dbg_mask;

	/*set default msg level*/
	unet->msg_enable = NETIF_MSG_DRV | NETIF_MSG_PROBE | NETIF_MSG_LINK;

	/*enable netif_xxx msgs*/
	if (dbg_mask & DEBUG_MASK_LVL0)
		unet->msg_enable |= NETIF_MSG_IFUP | NETIF_MSG_IFDOWN;
	if (dbg_mask & DEBUG_MASK_LVL1)
		unet->msg_enable |= NETIF_MSG_TX_ERR | NETIF_MSG_RX_ERR
			| NETIF_MSG_TX_QUEUED | NETIF_MSG_TX_DONE
			| NETIF_MSG_RX_STATUS;

	return n;
}

static ssize_t dbg_mask_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", data_msg_dbg_mask);
}

static DEVICE_ATTR(dbg_mask, 0644, dbg_mask_show, dbg_mask_store);

#define DBG0(x...) DBG(DEBUG_MASK_LVL0, x)
#define DBG1(x...) DBG(DEBUG_MASK_LVL1, x)
#define DBG2(x...) DBG(DEBUG_MASK_LVL2, x)

//++SSD_RIL:20120814: For CPU/Freq min default value
#define RMNET_USB_CPU_FREQ_MIN_DEFAULT_VALUE (s32)1000000
#define RMNET_USB_ONLINE_CPUS_MIN_DEFAULT_VALUE (s32)4
static struct pm_qos_request_list rmnet_usb_req_freq;
static struct pm_qos_request_list rmnet_usb_req_cpus;
static struct workqueue_struct* rmnet_usb_wq = NULL;
static struct delayed_work rmnet_usb_freq_enable_work;
static struct delayed_work rmnet_usb_freq_disable_work;
static int rmnet_usb_freq_enable_work_init = 0;
static int rmnet_usb_freq_disable_work_init = 0;
static int rmnet_usb_req_freq_active = 0;
static int rmnet_usb_req_cpus_active = 0;
static int rmnet_usb_req_enable = 0;
static int rnmet_usb_cpu_freq_enabled = 0;

static void rmnet_usb_freq_request(void);

static void rmnet_usb_freq_release(void)
{
	pr_info("%s\n", __func__);

	cancel_delayed_work_sync(&rmnet_usb_freq_enable_work);
	cancel_delayed_work_sync(&rmnet_usb_freq_disable_work);

	destroy_workqueue(rmnet_usb_wq);
	rmnet_usb_wq = NULL;

	if ( rmnet_usb_req_freq_active == 1 ) {
		pr_info("%s: pm_qos_remove_request: freq", __func__);
		pm_qos_remove_request(&rmnet_usb_req_freq);
		rmnet_usb_req_freq_active = 0;
	}
	if ( rmnet_usb_req_cpus_active == 1 ) {
		pr_info("%s: pm_qos_remove_request: cpus", __func__);
		pm_qos_remove_request(&rmnet_usb_req_cpus);
		rmnet_usb_req_cpus_active = 0;
	}
}

static void rmnet_usb_freq_enable(void)
{
	pr_info("%s\n", __func__);
	if ( rmnet_usb_req_freq_active == 0 && rmnet_usb_req_cpus_active == 0 ){
		rmnet_usb_freq_request();
	}

	if ( rmnet_usb_req_enable == 0 ) {
		rmnet_usb_req_enable = 1;
		if ( rmnet_usb_req_freq_active == 1 ) {
			pr_info("%s: pm_qos_update_request: freq", __func__);
			pm_qos_update_request(&rmnet_usb_req_freq, RMNET_USB_CPU_FREQ_MIN_DEFAULT_VALUE);
		}

		if ( rmnet_usb_req_cpus_active == 1 ) {
			pr_info("%s: pm_qos_update_request: cpus", __func__);
			pm_qos_update_request(&rmnet_usb_req_cpus, RMNET_USB_ONLINE_CPUS_MIN_DEFAULT_VALUE);
		}
	}
}

static void rmnet_usb_freq_disable(void)
{
	pr_info("%s\n", __func__);
	if ( rmnet_usb_req_enable == 1 ) {
		rmnet_usb_req_enable = 0;
		if ( rmnet_usb_req_freq_active == 1 ) {
			pr_info("%s: pm_qos_update_request: freq", __func__);
			pm_qos_update_request(&rmnet_usb_req_freq, (s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
		}

		if ( rmnet_usb_req_cpus_active == 1 ) {
			pr_info("%s: pm_qos_update_request: cpus", __func__);
			pm_qos_update_request(&rmnet_usb_req_cpus, (s32)PM_QOS_MIN_ONLINE_CPUS_DEFAULT_VALUE);
		}
	}
}

static void rmnet_usb_freq_enable_work_func(struct work_struct * work)
{
	pr_info("%s\n", __func__);
	rmnet_usb_freq_enable();
}

static void rmnet_usb_freq_disable_work_func(struct work_struct * work)
{
	pr_info("%s\n", __func__);
	rmnet_usb_freq_disable();
}

static void rmnet_usb_freq_timer_enable(void)
{
	pr_info("%s\n", __func__);
	cancel_delayed_work_sync(&rmnet_usb_freq_enable_work);
	cancel_delayed_work_sync(&rmnet_usb_freq_disable_work);
	queue_delayed_work(rmnet_usb_wq, &rmnet_usb_freq_enable_work, msecs_to_jiffies(10));
}

static void rmnet_usb_freq_timer_disable(void)
{
	pr_info("%s\n", __func__);
	cancel_delayed_work_sync(&rmnet_usb_freq_enable_work);
	cancel_delayed_work_sync(&rmnet_usb_freq_disable_work);
	queue_delayed_work(rmnet_usb_wq, &rmnet_usb_freq_disable_work, msecs_to_jiffies(10));
}

static void rmnet_usb_freq_request(void)
{
	pr_info("%s\n", __func__);
	if ( rmnet_usb_wq == NULL ) {
		rmnet_usb_wq = create_singlethread_workqueue("rmnet_usb_freq");
		pr_info("%s: create_singlethread_workqueue=[0x%p]\n", __func__, rmnet_usb_wq);
	}

	if ( rmnet_usb_freq_enable_work_init == 0 ) {
		INIT_DELAYED_WORK(&rmnet_usb_freq_enable_work, rmnet_usb_freq_enable_work_func);
		pr_info("%s: INIT_WORK: rmnet_usb_freq_enable_work\n", __func__);
		rmnet_usb_freq_enable_work_init = 1;
	}

	if ( rmnet_usb_freq_disable_work_init == 0 ) {
		INIT_DELAYED_WORK(&rmnet_usb_freq_disable_work, rmnet_usb_freq_disable_work_func);
		rmnet_usb_freq_disable_work_init = 1;
	}

	if ( rmnet_usb_req_freq_active == 0 ) {
		pr_info("%s: pm_qos_add_request: freq", __func__);
		pm_qos_add_request(&rmnet_usb_req_freq, PM_QOS_CPU_FREQ_MIN, (s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
		rmnet_usb_req_freq_active = 1;
	}

	if ( rmnet_usb_req_cpus_active == 0 ) {
		pr_info("%s: pm_qos_add_request: cpus", __func__);
		pm_qos_add_request(&rmnet_usb_req_cpus, PM_QOS_MIN_ONLINE_CPUS, (s32)PM_QOS_MIN_ONLINE_CPUS_DEFAULT_VALUE);
		rmnet_usb_req_cpus_active = 1;
	}

}
//--SSD_RIL

//++SSD_RIL:20120731: For tx/rx enable_hlt/disable_hlt
static int rnmet_usb_hlt_enabled = 0;
static int rnmet_usb_hlt_timer_enabled = 0;
static DEFINE_SPINLOCK(rmnet_usb_hlt_lock);

static void rmnet_usb_lp2_in_idle_timer_expired(unsigned long i)
{
//++SSD_RIL:20120814: For CPU/Freq min default value
	int is_enable_hlt = 0;
//--SSD_RIL
	unsigned long flags = 0;
	spin_lock_irqsave(&rmnet_usb_hlt_lock, flags);
	if ( rnmet_usb_hlt_timer_enabled == 1 ) {
		rnmet_usb_hlt_timer_enabled = 0;
		enable_hlt();
//++SSD_RIL:20120814: For CPU/Freq min default value
		is_enable_hlt = 1;
//--SSD_RIL
		pr_info("%s: rmnet hlt enable\n", __func__);
	}
	spin_unlock_irqrestore(&rmnet_usb_hlt_lock, flags);
//++SSD_RIL:20120814: For CPU/Freq min default value
	if (is_enable_hlt == 1 && rnmet_usb_cpu_freq_enabled == 1) {
		rmnet_usb_freq_timer_disable();
	}
//--SSD_RIL
}

static DEFINE_TIMER(rmnet_usb_lp2_in_idle_timer, rmnet_usb_lp2_in_idle_timer_expired, 0, 0);
//--SSD_RIL

static void rmnet_usb_setup(struct net_device *);
static int rmnet_ioctl(struct net_device *, struct ifreq *, int);

static int rmnet_usb_suspend(struct usb_interface *iface, pm_message_t message)
{
	struct usbnet		*unet;
	struct rmnet_ctrl_dev	*dev;
	int			time = 0;
	int			retval = 0;

	//htc_dbg
	if (get_radio_flag() & 0x0001)
		pr_info("%s intf %p\n", __func__, iface);

	unet = usb_get_intfdata(iface);
	if (!unet) {
		pr_err("%s:data device not found\n", __func__);
		retval = -ENODEV;
		goto fail;
	}

	dev = (struct rmnet_ctrl_dev *)unet->data[1];
	if (!dev) {
		dev_err(&iface->dev, "%s: ctrl device not found\n",
				__func__);
		retval = -ENODEV;
		goto fail;
	}

	retval = usbnet_suspend(iface, message);
	if (!retval) {
		if (message.event & PM_EVENT_SUSPEND) {
			time = usb_wait_anchor_empty_timeout(&dev->tx_submitted,
								1000);
			if (!time)
			{
				pr_info("%s :+usb_kill_anchored_urbs\n", __func__);
				usb_kill_anchored_urbs(&dev->tx_submitted);
				pr_info("%s :-usb_kill_anchored_urbs\n", __func__);
			}

			retval = rmnet_usb_ctrl_stop_rx(dev);
			iface->dev.power.power_state.event = message.event;
		}
		/*  TBD : do we need to set/clear usbnet->udev->reset_resume*/
	} else
		dev_err(&iface->dev,
			"%s: device is busy can not suspend\n", __func__);

fail:
	return retval;
}


static int rmnet_usb_resume(struct usb_interface *iface)
{
	int			retval = 0;
	int			oldstate;
	struct usbnet		*unet;
	struct rmnet_ctrl_dev	*dev;

	//htc_dbg
	if (get_radio_flag() & 0x0001)
	pr_info("%s intf %p\n", __func__, iface);
	unet = usb_get_intfdata(iface);
	if (!unet) {
		pr_err("%s:data device not found\n", __func__);
		retval = -ENODEV;
		goto fail;
	}

	dev = (struct rmnet_ctrl_dev *)unet->data[1];
	if (!dev) {
		dev_err(&iface->dev, "%s: ctrl device not found\n", __func__);
		retval = -ENODEV;
		goto fail;
	}
	oldstate = iface->dev.power.power_state.event;
	iface->dev.power.power_state.event = PM_EVENT_ON;

	retval = usbnet_resume(iface);
	if (!retval) {
		if (oldstate & PM_EVENT_SUSPEND)
			retval = rmnet_usb_ctrl_start(dev);
	}
fail:
	return retval;
}

int rmnet_usb_reset_resume(struct usb_interface *intf)
{
	pr_info("%s intf %p\n", __func__, intf);
	return rmnet_usb_resume(intf);
}


static int rmnet_usb_bind(struct usbnet *usbnet, struct usb_interface *iface)
{
	struct usb_host_endpoint	*endpoint = NULL;
	struct usb_host_endpoint	*bulk_in = NULL;
	struct usb_host_endpoint	*bulk_out = NULL;
	struct usb_host_endpoint	*int_in = NULL;
	int				status = 0;
	int				i;
	int				numends;
	int				iface_num;
	char 			rmnet_name[12];

	numends = iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			dev_err(&iface->dev, "%s: invalid endpoint %u\n",
				__func__, i);
			status = -EINVAL;
			goto out;
		}
		if (usb_endpoint_is_bulk_in(&endpoint->desc))
			bulk_in = endpoint;
		else if (usb_endpoint_is_bulk_out(&endpoint->desc))
			bulk_out = endpoint;
		else if (usb_endpoint_is_int_in(&endpoint->desc))
			int_in = endpoint;
	}

	if (!bulk_in || !bulk_out || !int_in) {
		dev_err(&iface->dev, "%s: invalid endpoints\n", __func__);
		status = -EINVAL;
		goto out;
	}
	usbnet->in = usb_rcvbulkpipe(usbnet->udev,
		bulk_in->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	usbnet->out = usb_sndbulkpipe(usbnet->udev,
		bulk_out->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	usbnet->status = int_in;

//++SSD_RIL:20120905: Fixed rmnet_usb index
	iface_num = iface->cur_altsetting->desc.bInterfaceNumber;
	sprintf(rmnet_name,	"rmnet_usb%d",iface_num-5);
	/*change name of net device to rmnet_usbx here*/
	//strlcpy(usbnet->net->name, "rmnet_usb%d", IFNAMSIZ);
	strlcpy(usbnet->net->name, rmnet_name, IFNAMSIZ);
//--SSD_RIL:20120905: Fixed rmnet_usb index

	/*TBD: update rx_urb_size, curently set to eth frame len by usbnet*/
out:
	return status;
}

static struct sk_buff *rmnet_usb_tx_fixup(struct usbnet *dev,
		struct sk_buff *skb, gfp_t flags)
{
	struct QMI_QOS_HDR_S	*qmih;
//++SSD_RIL:20120731: For tx/rx enable_hlt/disable_hlt
	if (rnmet_usb_hlt_enabled == 1 ) {
//++SSD_RIL:20120814: For CPU/Freq min default value
		int is_disable_flt = 0;
//--SSD_RIL
		unsigned long flags = 0;
		spin_lock_irqsave(&rmnet_usb_hlt_lock, flags);
		if ( rnmet_usb_hlt_timer_enabled == 0 ) {
			rnmet_usb_hlt_timer_enabled = 1;
			disable_hlt();
//++SSD_RIL:20120814: For CPU/Freq min default value
			is_disable_flt = 1;
//--SSD_RIL
			pr_info("%s: rmnet hlt disable\n", __func__);
		}
		del_timer(&rmnet_usb_lp2_in_idle_timer);
		mod_timer(&rmnet_usb_lp2_in_idle_timer, jiffies + msecs_to_jiffies(2000));
		spin_unlock_irqrestore(&rmnet_usb_hlt_lock, flags);
//++SSD_RIL:20120814: For CPU/Freq min default value
		if ( is_disable_flt == 1 && rnmet_usb_cpu_freq_enabled == 1 ) {
			rmnet_usb_freq_timer_enable();
		}
//--SSD_RIL
	}
//--SSD_RIL
	if (test_bit(RMNET_MODE_QOS, &dev->data[0])) {
		qmih = (struct QMI_QOS_HDR_S *)
		skb_push(skb, sizeof(struct QMI_QOS_HDR_S));
		qmih->version = 1;
		qmih->flags = 0;
		qmih->flow_id = skb->mark;
	 }

	DBG1("[%s] Tx packet #%lu len=%d mark=0x%x\n",
	    dev->net->name, dev->net->stats.tx_packets, skb->len, skb->mark);

	return skb;
}

static __be16 rmnet_ip_type_trans(struct sk_buff *skb,
	struct net_device *dev)
{
	__be16	protocol = 0;

	skb->dev = dev;

	switch (skb->data[0] & 0xf0) {
	case 0x40:
		protocol = htons(ETH_P_IP);
		break;
	case 0x60:
		protocol = htons(ETH_P_IPV6);
		break;
	default:
		pr_err("[%s] rmnet_recv() L3 protocol decode error: 0x%02x",
		       dev->name, skb->data[0] & 0xf0);
	}

	return protocol;
}

static int rmnet_usb_rx_fixup(struct usbnet *dev,
	struct sk_buff *skb)
{
//++SSD_RIL:20120731: For tx/rx enable_hlt/disable_hlt
	if (rnmet_usb_hlt_enabled == 1 ) {
//++SSD_RIL:20120814: For CPU/Freq min default value
		int is_disable_flt = 0;
//--SSD_RIL
		unsigned long flags = 0;
		spin_lock_irqsave(&rmnet_usb_hlt_lock, flags);
		if ( rnmet_usb_hlt_timer_enabled == 0 ) {
			rnmet_usb_hlt_timer_enabled = 1;
			disable_hlt();
//++SSD_RIL:20120814: For CPU/Freq min default value
			is_disable_flt = 1;
//--SSD_RIL
			pr_info("%s: rmnet hlt disable\n", __func__);
		}
		del_timer(&rmnet_usb_lp2_in_idle_timer);
		mod_timer(&rmnet_usb_lp2_in_idle_timer, jiffies + msecs_to_jiffies(2000));
		spin_unlock_irqrestore(&rmnet_usb_hlt_lock, flags);
//++SSD_RIL:20120814: For CPU/Freq min default value
		if ( is_disable_flt == 1 && rnmet_usb_cpu_freq_enabled == 1 ) {
			rmnet_usb_freq_timer_enable();
		}
//--SSD_RIL
	}
//--SSD_RIL

	if (test_bit(RMNET_MODE_LLP_IP, &dev->data[0]))
		skb->protocol = rmnet_ip_type_trans(skb, dev->net);
	else /*set zero for eth mode*/
		skb->protocol = 0;

	DBG1("[%s] Rx packet #%lu len=%d\n",
		dev->net->name, dev->net->stats.rx_packets, skb->len);

	return 1;
}

static int rmnet_usb_manage_power(struct usbnet *dev, int on)
{
	dev->intf->needs_remote_wakeup = on;
	return 0;
}

static int rmnet_change_mtu(struct net_device *dev, int new_mtu)
{
	if (0 > new_mtu || RMNET_DATA_LEN < new_mtu)
		return -EINVAL;

	DBG0("[%s] MTU change: old=%d new=%d\n", dev->name, dev->mtu, new_mtu);

	dev->mtu = new_mtu;

	return 0;
}

static struct net_device_stats *rmnet_get_stats(struct net_device *dev)
{
		return &dev->stats;
}

static const struct net_device_ops rmnet_usb_ops_ether = {
	.ndo_open = usbnet_open,
	.ndo_stop = usbnet_stop,
	.ndo_start_xmit = usbnet_start_xmit,
	.ndo_get_stats = rmnet_get_stats,
	/*.ndo_set_multicast_list = rmnet_set_multicast_list,*/
	.ndo_tx_timeout = usbnet_tx_timeout,
	.ndo_do_ioctl = rmnet_ioctl,
	.ndo_change_mtu = usbnet_change_mtu,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static const struct net_device_ops rmnet_usb_ops_ip = {
	.ndo_open = usbnet_open,
	.ndo_stop = usbnet_stop,
	.ndo_start_xmit = usbnet_start_xmit,
	.ndo_get_stats = rmnet_get_stats,
	/*.ndo_set_multicast_list = rmnet_set_multicast_list,*/
	.ndo_tx_timeout = usbnet_tx_timeout,
	.ndo_do_ioctl = rmnet_ioctl,
	.ndo_change_mtu = rmnet_change_mtu,
	.ndo_set_mac_address = 0,
	.ndo_validate_addr = 0,
};


static int rmnet_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct usbnet	*unet = netdev_priv(dev);
	u32		old_opmode;
	int		prev_mtu = dev->mtu;
	int		rc = 0;

	old_opmode = unet->data[0]; /*data[0] saves operation mode*/
	/* Process IOCTL command */
	switch (cmd) {
	case RMNET_IOCTL_SET_LLP_ETHERNET:	/*Set Ethernet protocol*/
		/* Perform Ethernet config only if in IP mode currently*/
		if (test_bit(RMNET_MODE_LLP_IP, &unet->data[0])) {
			ether_setup(dev);
			random_ether_addr(dev->dev_addr);
			dev->mtu = prev_mtu;
			dev->netdev_ops = &rmnet_usb_ops_ether;
			clear_bit(RMNET_MODE_LLP_IP, &unet->data[0]);
			set_bit(RMNET_MODE_LLP_ETH, &unet->data[0]);
			DBG0("[%s] rmnet_ioctl(): set Ethernet protocol mode\n",
					dev->name);
		}
		break;

	case RMNET_IOCTL_SET_LLP_IP:		/* Set RAWIP protocol*/
		/* Perform IP config only if in Ethernet mode currently*/
		if (test_bit(RMNET_MODE_LLP_ETH, &unet->data[0])) {

			/* Undo config done in ether_setup() */
			dev->header_ops = 0;  /* No header */
			dev->type = ARPHRD_RAWIP;
			dev->hard_header_len = 0;
			dev->mtu = prev_mtu;
			dev->addr_len = 0;
			dev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
			dev->needed_headroom = HEADROOM_FOR_QOS;
			dev->netdev_ops = &rmnet_usb_ops_ip;
			clear_bit(RMNET_MODE_LLP_ETH, &unet->data[0]);
			set_bit(RMNET_MODE_LLP_IP, &unet->data[0]);
			DBG0("[%s] rmnet_ioctl(): set IP protocol mode\n",
					dev->name);
		}
		break;

	case RMNET_IOCTL_GET_LLP:	/* Get link protocol state */
		ifr->ifr_ifru.ifru_data = (void *)(unet->data[0]
						& (RMNET_MODE_LLP_ETH
						| RMNET_MODE_LLP_IP));
		break;

	case RMNET_IOCTL_SET_QOS_ENABLE:	/* Set QoS header enabled*/
		set_bit(RMNET_MODE_QOS, &unet->data[0]);
		DBG0("[%s] rmnet_ioctl(): set QMI QOS header enable\n",
				dev->name);
		break;

	case RMNET_IOCTL_SET_QOS_DISABLE:	/* Set QoS header disabled */
		clear_bit(RMNET_MODE_QOS, &unet->data[0]);
		DBG0("[%s] rmnet_ioctl(): set QMI QOS header disable\n",
				dev->name);
		break;

	case RMNET_IOCTL_GET_QOS:		/* Get QoS header state */
		ifr->ifr_ifru.ifru_data = (void *)(unet->data[0]
						& RMNET_MODE_QOS);
		break;

	case RMNET_IOCTL_GET_OPMODE:		/* Get operation mode*/
		ifr->ifr_ifru.ifru_data = (void *)unet->data[0];
		break;

	case RMNET_IOCTL_OPEN:			/* Open transport port */
		rc = usbnet_open(dev);
		DBG0("[%s] rmnet_ioctl(): open transport port\n", dev->name);
		break;

	case RMNET_IOCTL_CLOSE:			/* Close transport port*/
		rc = usbnet_stop(dev);
		DBG0("[%s] rmnet_ioctl(): close transport port\n", dev->name);
		break;

	default:
		dev_err(&unet->intf->dev, "[%s] error: "
			"rmnet_ioct called for unsupported cmd[%d]",
			dev->name, cmd);
		return -EINVAL;
	}

	DBG2("[%s] %s: cmd=0x%x opmode old=0x%08x new=0x%08lx\n",
		dev->name, __func__, cmd, old_opmode, unet->data[0]);

	return rc;
}

static void rmnet_usb_setup(struct net_device *dev)
{
	/* Using Ethernet mode by default */
	dev->netdev_ops = &rmnet_usb_ops_ether;

	/* set this after calling ether_setup */
	dev->mtu = RMNET_DATA_LEN;

	dev->needed_headroom = HEADROOM_FOR_QOS;
	random_ether_addr(dev->dev_addr);
	dev->watchdog_timeo = 1000; /* 10 seconds? */
}

static int rmnet_usb_data_status(struct seq_file *s, void *unused)
{
	struct usbnet *unet = s->private;

	seq_printf(s, "RMNET_MODE_LLP_IP:  %d\n",
			test_bit(RMNET_MODE_LLP_IP, &unet->data[0]));
	seq_printf(s, "RMNET_MODE_LLP_ETH: %d\n",
			test_bit(RMNET_MODE_LLP_ETH, &unet->data[0]));
	seq_printf(s, "RMNET_MODE_QOS:     %d\n",
			test_bit(RMNET_MODE_QOS, &unet->data[0]));
	seq_printf(s, "Net MTU:            %u\n", unet->net->mtu);
	seq_printf(s, "rx_urb_size:        %u\n", unet->rx_urb_size);
	seq_printf(s, "rx skb q len:       %u\n", unet->rxq.qlen);
	seq_printf(s, "rx skb done q len:  %u\n", unet->done.qlen);
	seq_printf(s, "rx errors:          %lu\n", unet->net->stats.rx_errors);
	seq_printf(s, "rx over errors:     %lu\n",
			unet->net->stats.rx_over_errors);
	seq_printf(s, "rx length errors:   %lu\n",
			unet->net->stats.rx_length_errors);
	seq_printf(s, "rx packets:         %lu\n", unet->net->stats.rx_packets);
	seq_printf(s, "rx bytes:           %lu\n", unet->net->stats.rx_bytes);
	seq_printf(s, "tx skb q len:       %u\n", unet->txq.qlen);
	seq_printf(s, "tx errors:          %lu\n", unet->net->stats.tx_errors);
	seq_printf(s, "tx packets:         %lu\n", unet->net->stats.tx_packets);
	seq_printf(s, "tx bytes:           %lu\n", unet->net->stats.tx_bytes);
	seq_printf(s, "suspend count:      %d\n", unet->suspend_count);
	seq_printf(s, "EVENT_DEV_OPEN:     %d\n",
			test_bit(EVENT_DEV_OPEN, &unet->flags));
	seq_printf(s, "EVENT_TX_HALT:      %d\n",
			test_bit(EVENT_TX_HALT, &unet->flags));
	seq_printf(s, "EVENT_RX_HALT:      %d\n",
			test_bit(EVENT_RX_HALT, &unet->flags));
	seq_printf(s, "EVENT_RX_MEMORY:    %d\n",
			test_bit(EVENT_RX_MEMORY, &unet->flags));
	seq_printf(s, "EVENT_DEV_ASLEEP:   %d\n",
			test_bit(EVENT_DEV_ASLEEP, &unet->flags));

	return 0;
}

static int rmnet_usb_data_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, rmnet_usb_data_status, inode->i_private);
}

const struct file_operations rmnet_usb_data_fops = {
	.open = rmnet_usb_data_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int rmnet_usb_data_debugfs_init(struct usbnet *unet)
{
	struct dentry *rmnet_usb_data_dbg_root;
	struct dentry *rmnet_usb_data_dentry;

	rmnet_usb_data_dbg_root = debugfs_create_dir(unet->net->name, NULL);
	if (!rmnet_usb_data_dbg_root || IS_ERR(rmnet_usb_data_dbg_root))
		return -ENODEV;

	rmnet_usb_data_dentry = debugfs_create_file("status",
		S_IRUGO | S_IWUSR,
		rmnet_usb_data_dbg_root, unet,
		&rmnet_usb_data_fops);

	if (!rmnet_usb_data_dentry) {
		debugfs_remove_recursive(rmnet_usb_data_dbg_root);
		return -ENODEV;
	}

	unet->data[2] = (unsigned long)rmnet_usb_data_dbg_root;

	return 0;
}

static void rmnet_usb_data_debugfs_cleanup(struct usbnet *unet)
{
	struct dentry *root = (struct dentry *)unet->data[2];

	debugfs_remove_recursive(root);
	unet->data[2] = 0;
}

static int rmnet_usb_probe(struct usb_interface *iface,
		const struct usb_device_id *prod)
{
	struct usbnet		*unet;
	struct driver_info	*info;
	struct usb_device	*udev;
	unsigned int		iface_num;
	static int		first_rmnet_iface_num = -EINVAL;
	int			status = 0;

//++SSD_RIL:20120731: For tx/rx enable_hlt/disable_hlt
	if( machine_is_evitareul() ) {
		rnmet_usb_hlt_enabled = 1;
		pr_info("%s:rnmet_usb_hlt_enabled = 1\n", __func__);
	}
//--SSD_RIL
//++SSD_RIL:20120814: For CPU/Freq min default value
	if ( rnmet_usb_hlt_enabled == 1 && get_radio_flag() & 0x0002 ) {
		rnmet_usb_cpu_freq_enabled = 1;
	}
//--SSD_RIL

	iface_num = iface->cur_altsetting->desc.bInterfaceNumber;
	if (iface->num_altsetting != 1) {
		dev_err(&iface->dev, "%s invalid num_altsetting %u\n",
			__func__, iface->num_altsetting);
		status = -EINVAL;
		goto out;
	}

	info = (struct driver_info *)prod->driver_info;
	if (!test_bit(iface_num, &info->data))
		return -ENODEV;

	status = usbnet_probe(iface, prod);
	if (status < 0) {
		dev_err(&iface->dev, "usbnet_probe failed %d\n", status);
		goto out;
	}
	unet = usb_get_intfdata(iface);

	/*set rmnet operation mode to eth by default*/
	set_bit(RMNET_MODE_LLP_ETH, &unet->data[0]);

	/*update net device*/
	rmnet_usb_setup(unet->net);

	/*create /sys/class/net/rmnet_usbx/dbg_mask*/
	status = device_create_file(&unet->net->dev, &dev_attr_dbg_mask);
	if (status)
		goto out;

	if (first_rmnet_iface_num == -EINVAL)
		first_rmnet_iface_num = iface_num;

	/*save control device intstance */
	unet->data[1] = (unsigned long)ctrl_dev	\
			[iface_num - first_rmnet_iface_num];

	status = rmnet_usb_ctrl_probe(iface, unet->status,
		(struct rmnet_ctrl_dev *)unet->data[1]);
	if (status)
		goto out;

	status = rmnet_usb_data_debugfs_init(unet);
	if (status)
		dev_dbg(&iface->dev, "mode debugfs file is not available\n");

	udev = unet->udev;

	if (udev->parent && !udev->parent->parent) {
		/* allow modem and roothub to wake up suspended system */
		device_set_wakeup_enable(&udev->dev, 1);
		device_set_wakeup_enable(&udev->parent->dev, 1);

		/* set default autosuspend timeout for modem and roothub */
		pm_runtime_set_autosuspend_delay(&udev->dev, 1000);
		dev_err(&udev->dev, "pm_runtime_set_autosuspend_delay 1000\n");
		pm_runtime_set_autosuspend_delay(&udev->parent->dev, 200);
		dev_err(&udev->parent->dev, "pm_runtime_set_autosuspend_delay 200\n");
	}

out:
//++SSD_RIL:20120814: For CPU/Freq min default value
	if ( rnmet_usb_cpu_freq_enabled == 1 ) {
		rmnet_usb_freq_request();
	}
//--SSD_RIL


//++SSD_RIL:20121017: get -71 but already register rmnet netdev
if (status == -EPROTO && already_register_rmNET  ) {
	pr_info("%s fail ,status = %d,unregister_netdev \n", __func__,status);
	//unregister_netdev(unet->net);
	//usb_put_dev(unet->udev);
	usbnet_disconnect(iface);
}
//--SSD_RIL:20121017: get -71 but already register rmnet netdev

	return status;
}

static void rmnet_usb_disconnect(struct usb_interface *intf)
{
	struct usbnet		*unet;
	struct rmnet_ctrl_dev	*dev;

	unet = usb_get_intfdata(intf);
	if (!unet) {
		dev_err(&intf->dev, "%s:data device not found\n", __func__);
		return;
	}

	device_set_wakeup_enable(&unet->udev->dev, 0);
	rmnet_usb_data_debugfs_cleanup(unet);

	dev = (struct rmnet_ctrl_dev *)unet->data[1];
	if (!dev) {
		dev_err(&intf->dev, "%s:ctrl device not found\n", __func__);
		return;
	}
	unet->data[0] = 0;
	unet->data[1] = 0;
	rmnet_usb_ctrl_disconnect(dev);
	device_remove_file(&unet->net->dev, &dev_attr_dbg_mask);
	usbnet_disconnect(intf);

//++SSD_RIL:20120814: For CPU/Freq min default value
	if ( rnmet_usb_cpu_freq_enabled == 1 ) {
		rmnet_usb_freq_release();
	}
//--SSD_RIL
}

/*bit position represents interface number*/
#define PID9034_IFACE_MASK	0xF0
#define PID9048_IFACE_MASK	0x1E0
#define PID904C_IFACE_MASK	0x1C0

static const struct driver_info rmnet_info_pid9034 = {
	.description   = "RmNET net device",
	.bind          = rmnet_usb_bind,
	.tx_fixup      = rmnet_usb_tx_fixup,
	.rx_fixup      = rmnet_usb_rx_fixup,
	.manage_power  = rmnet_usb_manage_power,
	.data          = PID9034_IFACE_MASK,
};

static const struct driver_info rmnet_info_pid9048 = {
	.description   = "RmNET net device",
	.bind          = rmnet_usb_bind,
	.tx_fixup      = rmnet_usb_tx_fixup,
	.rx_fixup      = rmnet_usb_rx_fixup,
	.manage_power  = rmnet_usb_manage_power,
	.data          = PID9048_IFACE_MASK,
};

static const struct driver_info rmnet_info_pid904c = {
	.description   = "RmNET net device",
	.bind          = rmnet_usb_bind,
	.tx_fixup      = rmnet_usb_tx_fixup,
	.rx_fixup      = rmnet_usb_rx_fixup,
	.manage_power  = rmnet_usb_manage_power,
	.data          = PID904C_IFACE_MASK,
};

static const struct usb_device_id vidpids[] = {
	{
		USB_DEVICE(0x05c6, 0x9034), /* MDM9x15*/
		.driver_info = (unsigned long)&rmnet_info_pid9034,
	},
	{
		USB_DEVICE(0x05c6, 0x9048), /* MDM9x15*/
		.driver_info = (unsigned long)&rmnet_info_pid9048,
	},
	{
		USB_DEVICE(0x05c6, 0x904c), /* MDM9x15*/
		.driver_info = (unsigned long)&rmnet_info_pid904c,
	},

	{ }, /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, vidpids);

static struct usb_driver rmnet_usb = {
	.name       = "rmnet_usb",
	.id_table   = vidpids,
	.probe      = rmnet_usb_probe,
	.disconnect = rmnet_usb_disconnect,
	.suspend    = rmnet_usb_suspend,
	.resume     = rmnet_usb_resume,
	.reset_resume     = rmnet_usb_reset_resume,
	.supports_autosuspend = true,
};

static int __init rmnet_usb_init(void)
{
	int	retval;

	retval = usb_register(&rmnet_usb);
	if (retval) {
		err("usb_register failed: %d", retval);
		return retval;
	}
	/* initialize rmnet ctrl device here*/
	retval = rmnet_usb_ctrl_init();
	if (retval) {
		usb_deregister(&rmnet_usb);
		err("rmnet_usb_cmux_init failed: %d", retval);
		return retval;
	}

	return 0;
}
module_init(rmnet_usb_init);

static void __exit rmnet_usb_exit(void)
{
	rmnet_usb_ctrl_exit();
	usb_deregister(&rmnet_usb);
}
module_exit(rmnet_usb_exit);

MODULE_DESCRIPTION("msm rmnet usb device");
MODULE_LICENSE("GPL v2");
