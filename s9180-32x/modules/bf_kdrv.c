/*******************************************************************************
 * BAREFOOT NETWORKS CONFIDENTIAL & PROPRIETARY
 *
 * Copyright (c) 2015-2016 Barefoot Networks, Inc.

 * All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property of
 * Barefoot Networks, Inc. and its suppliers, if any. The intellectual and
 * technical concepts contained herein are proprietary to Barefoot Networks,
 * Inc.
 * and its suppliers and may be covered by U.S. and Foreign Patents, patents in
 * process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material is
 * strictly forbidden unless prior written permission is obtained from
 * Barefoot Networks, Inc.
 *
 * No warranty, explicit or implicit is provided, unless granted under a
 * written agreement with Barefoot Networks, Inc.
 *
 * $Id: $
 *
 ******************************************************************************/
/**
 *
 * GPL LICENSE SUMMARY
 *
 *   Copyright(c) 2015 Barefoot Networks. All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the...
 *
 **/

/* bf_drv kernel module
 *
 * This is kernel mode driver for Tofino chip. 
 * Provides user space mmap service and user space "wait for interrupt"
 * and "enable interrupt" services.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/msi.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/cdev.h>
#include <linux/aer.h>
#include <linux/string.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
//#error unsupported linux kernel version
#endif

/* TBD: Need to build with CONFIG_PCI_MSI */
extern int pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec);
extern int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec, int maxvec);

#define PCI_VENDOR_ID_BF                0x1d1c
#define TOFINO_DEV_ID_A0                0x01
#define TOFINO_DEV_ID_B0                0x10

#ifndef PCI_MSIX_ENTRY_SIZE
#define PCI_MSIX_ENTRY_SIZE             16
#define PCI_MSIX_ENTRY_LOWER_ADDR       0
#define PCI_MSIX_ENTRY_UPPER_ADDR       4
#define PCI_MSIX_ENTRY_DATA             8
#define PCI_MSIX_ENTRY_VECTOR_CTRL      12
#define PCI_MSIX_ENTRY_CTRL_MASKBIT     1
#endif

#define BF_CLASS_NAME   "bf"
#define BF_MAX_DEVICE_CNT 256
#define BF_INTR_MODE_NONE_NAME "none"
#define BF_INTR_MODE_LEGACY_NAME "legacy"
#define BF_INTR_MODE_MSI_NAME "msi"
#define BF_INTR_MODE_MSIX_NAME "msix"
#define BF_MAX_BAR_MAPS   6
#define BF_MSIX_ENTRY_CNT 128 /* TBD  make it 512 */
#define BF_MSI_ENTRY_CNT  2

/* interrupt mode */
enum bf_intr_mode {
  BF_INTR_MODE_NONE = 0,
  BF_INTR_MODE_LEGACY,
  BF_INTR_MODE_MSI,
  BF_INTR_MODE_MSIX
};

/* device memory */
struct bf_dev_mem {
  const char              *name;
  phys_addr_t             addr;
  resource_size_t         size;
  void __iomem            *internal_addr;
};

/* device information */
struct bf_dev_info {
  struct module           *owner;
  struct device           *dev;
  int                     minor;
  atomic_t                event[BF_MSIX_ENTRY_CNT];
  wait_queue_head_t       wait;
  const char              *version;
  struct bf_dev_mem       mem[BF_MAX_BAR_MAPS];
  struct msix_entry       *msix_entries;
  long                    irq;      /* first irq vector */
  int                     num_irq;  /* number of irq vectors */
  unsigned long           irq_flags;/* sharable ?? */
  int                     pci_error_state; /* was there a pci bus error */
};

/* cookie to be passed to IRQ handler, useful especially with MSIX */
struct bf_int_vector {
  struct bf_pci_dev *bf_dev;
  int int_vec_offset;
};


/**
 * A structure describing the private information for a BF pcie device.
 */
struct bf_pci_dev {
	struct bf_dev_info info;
	struct pci_dev *pdev;
  struct cdev cdev;
	enum   bf_intr_mode mode;
	u8     instance;
	char   name[16];
  struct bf_int_vector bf_int_vec[BF_MSIX_ENTRY_CNT];
};

/* Keep any global information here that must survive even after the
 * bf_pci_dev is free-ed up.
 */
struct bf_global {
  struct bf_pci_dev *bfdev ;
  struct fasync_struct *async_queue;
};

static int bf_major;
static int bf_minor[BF_MAX_DEVICE_CNT] = {0};
static struct class *bf_class = NULL;
static char *intr_mode = NULL;
static enum bf_intr_mode bf_intr_mode_default = BF_INTR_MODE_MSI;
static spinlock_t bf_nonisr_lock;
/* dev->minor should index into this array */
static struct bf_global bf_global[BF_MAX_DEVICE_CNT];

/* a pool of minor numbers is maintained */
/* return the first available minor number */
static int bf_get_next_minor_no(int *minor)
{
  int i;

  spin_lock(&bf_nonisr_lock);
  for(i = 0; i < BF_MAX_DEVICE_CNT; i++) {
    if (bf_minor[i] == 0) {
      *minor = i;
      bf_minor[i] = 1; /* mark it as taken */
      spin_unlock(&bf_nonisr_lock);
      return 0;
    }
  }
  *minor = -1;
  spin_unlock(&bf_nonisr_lock);
  return -1;
}

/* return a minor number back to the pool  for recycling */
static int bf_return_minor_no(int minor)
{
  int err;

  spin_lock(&bf_nonisr_lock);
  if (bf_minor[minor] == 0) { /* was already returned */
    err = -1; /* don't change anything, but return error */
  } else {
    bf_minor[minor] = 0; /* mark it as available */
    err = 0;
  }
  spin_unlock(&bf_nonisr_lock);
  return err;
}

static inline struct bf_pci_dev *bf_get_pci_dev(struct bf_dev_info *info)
{
	return container_of(info, struct bf_pci_dev, info);
}

/*
 * It masks the msix on/off of generating MSI-X messages.
 */
static void
bf_msix_mask_irq(struct msi_desc *desc, int32_t state)
{
	u32 mask_bits = desc->masked;
	unsigned offset = desc->msi_attrib.entry_nr * PCI_MSIX_ENTRY_SIZE +
                      PCI_MSIX_ENTRY_VECTOR_CTRL;

	if (state != 0)
		mask_bits &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	else
		mask_bits |= PCI_MSIX_ENTRY_CTRL_MASKBIT;

	if (mask_bits != desc->masked) {
		writel(mask_bits, desc->mask_base + offset);
		readl(desc->mask_base);
		desc->masked = mask_bits;
	}
}

/**
 * irqcontrol can be used to disable/enable interrupt from user space processes.
 *
 * @param bf_dev
 *  pointer to bf_pci_dev
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 *
 * @return
 *  - On success, 0.
 *  - On failure, a negative value.
 */
static int
bf_pci_irqcontrol(struct bf_pci_dev *bfdev, s32 irq_state)
{
	struct pci_dev *pdev = bfdev->pdev;

	pci_cfg_access_lock(pdev);
	if (bfdev->mode == BF_INTR_MODE_LEGACY)
		pci_intx(pdev, !!irq_state);

	else if (bfdev->mode == BF_INTR_MODE_MSIX) {
		struct msi_desc *desc;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
		list_for_each_entry(desc, &pdev->msi_list, list)
			bf_msix_mask_irq(desc, irq_state);
#else
		for_each_pci_msi_entry(desc, pdev)
			bf_msix_mask_irq(desc, irq_state);
#endif
	}
	pci_cfg_access_unlock(pdev);

	return 0;
}

/**
 * interrupt handler which will check if the interrupt is from the right
 * device. If so, disable it here and will be enabled later.
 */
static irqreturn_t bf_pci_irqhandler(int irq, struct bf_pci_dev *bfdev)
{
	/* Legacy mode need to mask in hardware */
	if (bfdev->mode == BF_INTR_MODE_LEGACY &&
	    !pci_check_and_mask_intx(bfdev->pdev))
		return IRQ_NONE;

	/* NOTE : if bfdev->info.pci_error_state == 1, then do not access the
	 * device and return IRQ_NOTHANDLED.
	 */
	/* Message signal mode, no share IRQ and automasked */
	return IRQ_HANDLED;
}

/* Remap pci resources described by bar #pci_bar */
static int
bf_pci_setup_iomem(struct pci_dev *dev, struct bf_dev_info *info,
		               int n, int pci_bar, const char *name)
{
	unsigned long addr, len;
	void *internal_addr;

	if (sizeof(info->mem) / sizeof(info->mem[0]) <= n)
		return -EINVAL;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -1;
	internal_addr = pci_ioremap_bar(dev, pci_bar);
	if (internal_addr == NULL)
		return -1;
	info->mem[n].name = name;
	info->mem[n].addr = addr;
	info->mem[n].internal_addr = internal_addr;
	info->mem[n].size = len;
	return 0;
}

/* Unmap previously ioremap'd resources */
static void
bf_pci_release_iomem(struct bf_dev_info *info)
{
	int i;

	for (i = 0; i < BF_MAX_BAR_MAPS; i++) {
		if (info->mem[i].internal_addr)
			iounmap(info->mem[i].internal_addr);
	}
}

static int
bf_setup_bars(struct pci_dev *dev, struct bf_dev_info *info)
{
	int i, iom, ret;
	unsigned long flags;
	static const char *bar_names[BF_MAX_BAR_MAPS]  = {
		"BAR0", "BAR1", "BAR2", "BAR3", "BAR4", "BAR5",
	};

	iom = 0;

	for (i = 0; i < BF_MAX_BAR_MAPS; i++) {
		if (pci_resource_len(dev, i) != 0 &&
		    pci_resource_start(dev, i) != 0) {
			flags = pci_resource_flags(dev, i);
			if (flags & IORESOURCE_MEM) {
				ret = bf_pci_setup_iomem(dev, info, iom, i, bar_names[i]);
				if (ret != 0)
					return ret;
				iom++;
			}
		}
	}
	return (iom != 0) ? ret : -ENOENT;
}

static irqreturn_t bf_interrupt(int irq, void *bfdev_id)
{
  struct bf_pci_dev *bfdev = ((struct bf_int_vector *)bfdev_id)->bf_dev;
  int vect_off = ((struct bf_int_vector *)bfdev_id)->int_vec_offset;

  irqreturn_t ret = bf_pci_irqhandler(irq, bfdev);

  if (ret == IRQ_HANDLED)
    atomic_inc(&(bfdev->info.event[vect_off]));

  return ret;
}

struct bf_listener {
  struct bf_pci_dev *bfdev;
  s32 event_count[BF_MSIX_ENTRY_CNT];
  int minor;
};

static unsigned int bf_poll(struct file *filep, poll_table *wait)
{
  struct bf_listener *listener = (struct bf_listener *)filep->private_data;
  struct bf_pci_dev *bfdev = listener->bfdev;
  int i;
    
  if (!bfdev->info.irq)
    return -EIO;
    
  poll_wait(filep, &bfdev->info.wait, wait);

  for (i = 0; i < BF_MSIX_ENTRY_CNT; i++)
    if (listener->event_count[i] != atomic_read(&bfdev->info.event[i]))
      return POLLIN | POLLRDNORM;
  return 0;
}

static int bf_find_mem_index(struct vm_area_struct *vma)
{
  struct bf_pci_dev *bfdev = vma->vm_private_data;
  if (vma->vm_pgoff < BF_MAX_BAR_MAPS) {
    if (bfdev->info.mem[vma->vm_pgoff].size == 0)
      return -1;
    return (int)vma->vm_pgoff;
  }
  return -1;
}
    
static const struct vm_operations_struct bf_physical_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
  .access = generic_access_phys,
#endif
};

static int bf_mmap_physical(struct vm_area_struct *vma)
{
  struct bf_pci_dev *bfdev = vma->vm_private_data;
  int bar = bf_find_mem_index(vma);
  struct bf_dev_mem *mem;
  if (bar < 0)
    return -EINVAL;

  mem = bfdev->info.mem + bar;
   
  if (mem->addr & ~PAGE_MASK)
    return -ENODEV;
  if (vma->vm_end - vma->vm_start > mem->size)
    return -EINVAL;
    
  vma->vm_ops = &bf_physical_vm_ops;
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    
  /*
   * We cannot use the vm_iomap_memory() helper here,
   * because vma->vm_pgoff is the map index we looked
   * up above in bf_find_mem_index(), rather than an
   * actual page offset into the mmap.
   *
   * So we just do the physical mmap without a page
   * offset.
   */
 return remap_pfn_range(vma, vma->vm_start, mem->addr >> PAGE_SHIFT,
                        vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static int bf_mmap(struct file *filep, struct vm_area_struct *vma)
{
  struct bf_listener *listener = filep->private_data;
  struct bf_pci_dev *bfdev = listener->bfdev;
  int bar;
  unsigned long requested_pages, actual_pages;
   
  if (vma->vm_end < vma->vm_start)
    return -EINVAL;
    
  vma->vm_private_data = bfdev;
    
  bar = bf_find_mem_index(vma);
  if (bar < 0)
    return -EINVAL;
    
  requested_pages = vma_pages(vma);
  actual_pages = ((bfdev->info.mem[bar].addr & ~PAGE_MASK)
                   + bfdev->info.mem[bar].size + PAGE_SIZE -1) >> PAGE_SHIFT;
  if (requested_pages > actual_pages)
    return -EINVAL;
    
  return bf_mmap_physical(vma);
}

static int bf_fasync(int fd, struct file *filep, int mode)
{
  int minor;

  if (!filep->private_data) {
    return (-EINVAL);
  }
  minor = ((struct bf_listener *)filep->private_data)->minor;
  if (minor >= BF_MAX_DEVICE_CNT) {
    return (-EINVAL);
  }
  if (mode == 0 && &bf_global[minor].async_queue == NULL) {
    return 0; /* nothing to do */
  }
  return (fasync_helper(fd, filep, mode, &bf_global[minor].async_queue));
}

static int bf_open(struct inode *inode, struct file *filep)
{
  struct bf_pci_dev *bfdev;
  struct bf_listener *listener;
  int i;

  bfdev = container_of(inode->i_cdev, struct bf_pci_dev, cdev);
  listener = kmalloc(sizeof(*listener), GFP_KERNEL);
  if (listener) {
    listener->bfdev = bfdev;
    listener->minor = bfdev->info.minor;
    for (i = 0; i < BF_MSIX_ENTRY_CNT; i++)
      listener->event_count[i] = atomic_read(&bfdev->info.event[i]);
    filep->private_data = listener;
    return 0;
  } else {
    return(-ENOMEM);
  }
}

static int bf_release(struct inode *inode, struct file *filep)
{
  struct bf_listener *listener = filep->private_data;

  bf_fasync(-1, filep, 0); /* empty any process id in the notification list */
  kfree(listener);
  return 0;
}

/* user space support: make read() system call after poll() of select() */
static ssize_t bf_read(struct file *filep, char __user *buf,
                       size_t count, loff_t *ppos)
{
  struct bf_listener *listener = filep->private_data;
  struct bf_pci_dev *bfdev = listener->bfdev;
  int retval, event_count[BF_MSIX_ENTRY_CNT];
  int i, mismatch_found = 0;  /* OR of per vector mismatch */
  unsigned char cnt_match[BF_MSIX_ENTRY_CNT]; /* per vector mismatch */

  /* irq must be setup for read() to work */
  if (!bfdev->info.irq)
    return -EIO;

  /* ensure that there is enough space on user buffer for the given interrupt
   * mode */
  if (bfdev->mode == BF_INTR_MODE_MSIX) {
    if (count < sizeof(s32)*BF_MSIX_ENTRY_CNT)
      return -EINVAL;
    count = sizeof(s32)*BF_MSIX_ENTRY_CNT;
  } else if (bfdev->mode == BF_INTR_MODE_MSI) {
    if (count < sizeof(s32)*BF_MSI_ENTRY_CNT)
      return -EINVAL;
    count = sizeof(s32)*BF_MSI_ENTRY_CNT;
  } else {
    if (count < sizeof(s32))
      return -EINVAL;
    count = sizeof(s32);
  }
  count = sizeof(s32)*BF_MSIX_ENTRY_CNT;

  do {
    set_current_state(TASK_INTERRUPTIBLE);

    for (i = 0; i < (count/sizeof(s32)); i++) {
      event_count[i] = atomic_read(&(bfdev->info.event[i]));
      if (event_count[i] != listener->event_count[i]) {
        mismatch_found |= 1;
        cnt_match[i] = 1;
      } else {
        event_count[i] = 0;
        cnt_match[i] = 0;
      }
    }
    if (mismatch_found) {
      __set_current_state(TASK_RUNNING);
      if (copy_to_user(buf, &event_count, count))
        retval = -EFAULT;
      else { /* adjust the listener->event_count; */
        for (i = 0 ; i < BF_MSIX_ENTRY_CNT; i++) {
          if (cnt_match[i]) {
            listener->event_count[i] = event_count[i];
          }
        }
        retval = count;
      }
      break;
    }

    if (filep->f_flags & O_NONBLOCK) {
      retval = -EAGAIN;
      break;
    }

    if (signal_pending(current)) {
      retval = -ERESTARTSYS;
      break;
    }
    schedule();
  } while (1);

  __set_current_state(TASK_RUNNING);

  return retval;
}

/* user space is supposed to call this after it is done with interrupt 
 * processing
 */
static ssize_t bf_write(struct file *filep, const char __user *buf,
                        size_t count, loff_t *ppos)
{
  struct bf_listener *listener = filep->private_data;
  struct bf_pci_dev *bfdev = listener->bfdev;
  ssize_t ret;
  s32 int_en;

  if (!bfdev || !bfdev->info.irq)
    return -EIO;
  
  if (count != sizeof(s32))
    return -EINVAL;

  if (copy_from_user(&int_en, buf, count)) 
    return -EFAULT;

  /* clear pci_error_state */
  bfdev->info.pci_error_state = 0;

  ret = bf_pci_irqcontrol(bfdev, int_en);

  return ret ? ret : sizeof(s32);
}

static const struct file_operations bf_fops = {
  .owner          = THIS_MODULE,
  .open           = bf_open,
  .release        = bf_release,
  .read           = bf_read,
  .write          = bf_write,
  .mmap           = bf_mmap,
  .poll           = bf_poll,
  .fasync         = bf_fasync,
};

static int bf_major_init(struct bf_pci_dev *bfdev)
{
  static const char name[] = "bf";
  dev_t bf_dev = 0;
  int result;

  result = alloc_chrdev_region(&bf_dev, 0, BF_MAX_DEVICE_CNT, name);
  if (result)
    return result;

  result = -ENOMEM;
/* following code useful if we chose to allocate cdev here
 *
 *struct cdev *cdev = NULL;
 * cdev = cdev_alloc();
 * if (!cdev)
 *   goto fail_unregister;
 * cdev->ops = &bf_fops;
 * cdev->owner = THIS_MODULE;
 * kobject_set_name(&cdev->kobj, "%s", name);
 *  result = cdev_add(cdev, bf_dev, BF_MAX_DEVICE_CNT);
 */
  cdev_init(&bfdev->cdev, &bf_fops);
  bfdev->cdev.owner = THIS_MODULE;
  bfdev->cdev.ops = &bf_fops;

  result = cdev_add(&bfdev->cdev, bf_dev, BF_MAX_DEVICE_CNT);
  if (result)
    goto fail_dev_add;

  bf_major = MAJOR(bf_dev);
  return 0;

fail_dev_add:
  unregister_chrdev_region(bf_dev, BF_MAX_DEVICE_CNT);
  return result;
}

static void bf_major_cleanup(struct bf_pci_dev *bfdev)
{
  unregister_chrdev_region(MKDEV(bf_major, 0), BF_MAX_DEVICE_CNT);
  cdev_del(&bfdev->cdev);
}

static int bf_init_cdev(struct bf_pci_dev *bfdev)
{
  int ret;
  ret = bf_major_init(bfdev);
  if (ret)
   return ret;
  
  bf_class = class_create(THIS_MODULE, BF_CLASS_NAME);
  if (!bf_class) {
    printk(KERN_ERR "create_class failed for bf_dev\n");
    ret = -ENODEV;
    goto err_class_register;
  }
  return 0;

err_class_register:
  bf_major_cleanup(bfdev);
  return ret;
}

static void bf_remove_cdev(struct bf_pci_dev *bfdev)
{
  class_destroy(bf_class);
  bf_major_cleanup(bfdev);
}


/**
 * bf_register_device - register a new userspace mem device
 * @parent:     parent device
 * @bfdev:      bf pci device 
 *
 * returns zero on success or a negative error code.
 */
int bf_register_device(struct device *parent, struct bf_pci_dev *bfdev)
{
  struct bf_dev_info *info = &bfdev->info;
  int i, j, ret = 0;
  int minor;

  if (!parent || !info || !info->version)
    return -EINVAL;

  init_waitqueue_head(&info->wait);

  for (i = 0; i < BF_MSIX_ENTRY_CNT; i++) {
    atomic_set(&info->event[i], 0);
  }

  ret = bf_init_cdev(bfdev);
  if (ret) {
    printk(KERN_ERR "BF: device cdev creation failed\n");
    return ret;
  }

  if (bf_get_next_minor_no(&minor)) {
    return -EINVAL;
  }

  info->dev = device_create(bf_class, parent,
                            MKDEV(bf_major, minor), bfdev,
                            "bf%d", minor);
  if (!info->dev) {
    printk(KERN_ERR "BF: device creation failed\n");
    return -ENODEV;
  }

  info->minor = minor;

  /* bind ISRs and request interrupts */
  if (info->irq && (bfdev->mode != BF_INTR_MODE_NONE)) {
    /*
     * Note that we deliberately don't use devm_request_irq
     * here. The parent module can unregister the UIO device
     * and call pci_disable_msi, which requires that this
     * irq has been freed. However, the device may have open
     * FDs at the time of unregister and therefore may not be
     * freed until they are released.
     */
    if (bfdev->mode == BF_INTR_MODE_LEGACY) {
      ret = request_irq(info->irq, bf_interrupt,
                        info->irq_flags, bfdev->name,
                        (void *)&(bfdev->bf_int_vec[0]));
      if (ret) {
        printk(KERN_ERR "bf failed to request legacy irq %ld error %d\n",
               info->irq, ret);
        return ret;
      }
      printk(KERN_NOTICE "BF allocating legacy int vector %ld\n", info->irq);
    } else if (bfdev->mode == BF_INTR_MODE_MSIX) {
      for (i = 0; i < info->num_irq; i++) {
        ret = request_irq(info->msix_entries[i].vector, bf_interrupt,
                          info->irq_flags, bfdev->name,
                          (void *)&(bfdev->bf_int_vec[i]));
        if (ret) {
          /* undo all other previous bindings */
          printk(KERN_ERR "bf failed to request MSIX ret %d itr %d\n", ret, i);
          for (j = i - 1; j >= 0; j--) {
            free_irq(info->msix_entries[j].vector,
                     (void *)&(bfdev->bf_int_vec[j]));
          }
          return ret;
        } 
      }
      printk(KERN_NOTICE "BF allocating %d MSIx vectors from  %ld\n",
             info->num_irq, info->irq);
    } else if (bfdev->mode == BF_INTR_MODE_MSI) {
      for (i = 0; i < info->num_irq; i++) {
        ret = request_irq(info->irq + i, bf_interrupt,
                          info->irq_flags, bfdev->name,
                          (void *)&(bfdev->bf_int_vec[i]));
        if (ret) {
          /* undo all other previous bindings */
          printk(KERN_ERR "bf failed to request MSI ret %d itr %d\n", ret, i);
          for (j = i - 1; j >= 0; j--) {
            free_irq(info->irq + j, (void *)&(bfdev->bf_int_vec[j]));
          }
          return ret;
        } 
      }
      printk(KERN_NOTICE "BF allocating %d MSI vectors from  %ld\n",
             info->num_irq, info->irq);
    }
  }
  return 0;
}

/**
 * bf_unregister_device - register a new userspace mem device
 * @bfdev:      bf pci device 
 *
 * returns none
 */
void bf_unregister_device(struct bf_pci_dev *bfdev)
{
  struct bf_dev_info *info = &bfdev->info;
  int i;

  if (info->irq) {
    if (bfdev->mode == BF_INTR_MODE_LEGACY) {
      free_irq(info->irq, (void *)&(bfdev->bf_int_vec[0]));
    } else if (bfdev->mode == BF_INTR_MODE_MSIX) {
      for (i = 0; i < info->num_irq; i++) {
        free_irq(info->msix_entries[i].vector, (void *)&(bfdev->bf_int_vec[i]));
      }
    } else if (bfdev->mode == BF_INTR_MODE_MSI) {
      for (i = 0; i < info->num_irq; i++) {
        free_irq(info->irq + i, (void *)&(bfdev->bf_int_vec[i]));
      }
    }
  }
  device_destroy(bf_class, MKDEV(bf_major, info->minor));
  bf_remove_cdev(bfdev);
  bf_return_minor_no(info->minor);
  return;
}

static inline struct device *pci_dev_to_dev(struct pci_dev *pdev)
{
  return &pdev->dev;
}

static int
bf_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
  struct bf_pci_dev *bfdev;
  int err, pci_use_highmem;
  int i, num_irq;

  memset(bf_global, 0, sizeof(bf_global));

  bfdev = kzalloc(sizeof(struct bf_pci_dev), GFP_KERNEL);
  if (!bfdev)
    return -ENOMEM;

  /* init the cookies to be passed to ISRs */
  for (i = 0; i < BF_MSIX_ENTRY_CNT; i++) {
    bfdev->bf_int_vec[i].int_vec_offset = i;
    bfdev->bf_int_vec[i].bf_dev = bfdev;
  }

  /* initialize intr_mode to none */
  bfdev->mode = BF_INTR_MODE_NONE;

  /* clear pci_error_state */
  bfdev->info.pci_error_state = 0;

  /*
   * enable device
   */
  err = pci_enable_device(pdev);
  if (err != 0) {
    dev_err(&pdev->dev, "Cannot enable PCI device\n");
    goto fail_free;
  }

  /*
   * reserve device's PCI memory regions for use by this
   * module
   */
  err = pci_request_regions(pdev, "bf_umem");
  if (err != 0) {
    dev_err(&pdev->dev, "Cannot request regions\n");
    goto fail_pci_disable;
  }
  /* remap IO memory */
  err = bf_setup_bars(pdev, &bfdev->info);
  if (err != 0)
    goto fail_release_iomem;

  if (!dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(64)) &&
      !dma_set_coherent_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(64))) {
    pci_use_highmem = 1;
  } else {
    err = dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(32));
    if (err) {
      err = dma_set_coherent_mask(pci_dev_to_dev(pdev),
                                  DMA_BIT_MASK(32));
      if (err) {
        dev_err(pci_dev_to_dev(pdev), "No usable DMA "
                                      "configuration, aborting\n");
		    goto fail_release_iomem;
      }
    }
    pci_use_highmem = 0;
  }

  /* enable pci error reporting */
  /* for the current kernel version, kernel config must have set the followings:
   * CONFIG_PCIEPORTBUS=y and CONFIG_PCIEAER = y
   * we have pci_error_handlers defined that gets invoked by kernel AER module
   * upon detecting the pcie error on this device's addresses.
   * However, there seems no way that AER would pass the offending addresses
   * to the callback functions. AER logs the error messages on the console.
   * This driver's calback function send the SIGIO signal to the user space
   * to indicate the error condition.
   */
  pci_enable_pcie_error_reporting(pdev);

  /* enable bus mastering on the device */
  pci_set_master(pdev);

  /* fill in bfdev info */
  bfdev->info.version = "0.2";
  bfdev->info.owner = THIS_MODULE;
  bfdev->pdev = pdev;

  switch (bf_intr_mode_default) {
#ifdef CONFIG_PCI_MSI
  case BF_INTR_MODE_MSIX:
    /* Only 1 msi-x vector needed */
    bfdev->info.msix_entries = kcalloc(BF_MSIX_ENTRY_CNT,
                                       sizeof(struct msix_entry), GFP_KERNEL);
    if (!bfdev->info.msix_entries) {
      err = -ENOMEM;
      goto fail_clear_pci_master;
    }
    for (i = 0; i < BF_MSIX_ENTRY_CNT; i++) {
      bfdev->info.msix_entries[i].entry= i;
    }
    num_irq = pci_enable_msix_range(pdev, bfdev->info.msix_entries,
                                    BF_MSIX_ENTRY_CNT, BF_MSIX_ENTRY_CNT);
    if (num_irq == BF_MSIX_ENTRY_CNT) {
	    dev_dbg(&pdev->dev, "using MSI-X");
      bfdev->info.num_irq = num_irq;
		  bfdev->info.irq = bfdev->info.msix_entries[0].vector;
		  bfdev->mode = BF_INTR_MODE_MSIX;
      printk(KERN_DEBUG "bf using %d MSIX irq from %ld\n", num_irq,
             bfdev->info.irq);
		  break;
    } else {
      if (num_irq)
        pci_disable_msix(pdev);
      kfree(bfdev->info.msix_entries);
      bfdev->info.msix_entries = NULL;
      printk(KERN_ERR "bf error allocating MSIX vectors. Trying MSI...\n");
      /* and, fall back to MSI */
    }
    /* ** intentional no-break */
  case BF_INTR_MODE_MSI:
    num_irq = pci_enable_msi_range(pdev, BF_MSI_ENTRY_CNT, BF_MSI_ENTRY_CNT);
    if (num_irq > 0) {
      dev_dbg(&pdev->dev, "using MSI");
      bfdev->info.num_irq = num_irq;
		  bfdev->info.irq = pdev->irq;
		  bfdev->mode = BF_INTR_MODE_MSI;
      printk(KERN_DEBUG "bf using %d MSI irq from %ld\n", bfdev->info.num_irq,
             bfdev->info.irq);
      break;
    }
#endif /* CONFIG_PCI_MSI */
    /* fall back to Legacy Interrupt, intentional no-break */

  case BF_INTR_MODE_LEGACY:
    if (pci_intx_mask_supported(pdev)) {
      dev_dbg(&pdev->dev, "using INTX");
      bfdev->info.irq_flags = IRQF_SHARED;
      bfdev->info.irq = pdev->irq;
      bfdev->mode = BF_INTR_MODE_LEGACY;
      printk(KERN_DEBUG "bf using LEGACY irq %ld\n", bfdev->info.irq);
      break;
    }
    dev_notice(&pdev->dev, "PCI INTx mask not supported\n");
    /* fall back to no Interrupt, intentional no-break */
  case BF_INTR_MODE_NONE:
    bfdev->info.irq = 0;
    bfdev->info.num_irq = 0;
    bfdev->mode = BF_INTR_MODE_NONE;
    break;

  default:
    dev_err(&pdev->dev, "invalid IRQ mode %u", bf_intr_mode_default);
    err = -EINVAL;
    goto fail_clear_pci_master;
  }

  /* register bf driver */
  err = bf_register_device(&pdev->dev, bfdev);
  if (err != 0)
    goto fail_release_irq;
  sprintf(bfdev->name, "bf_%d", bfdev->info.minor);
  pci_set_drvdata(pdev, bfdev);

  bf_global[bfdev->info.minor].async_queue = NULL;

  dev_info(&pdev->dev, "bf device %d registered with irq %ld\n",
  bfdev->instance, bfdev->info.irq);
  printk(KERN_ALERT "bf probe ok\n");
  return 0;

fail_release_irq:
  if (bfdev->mode == BF_INTR_MODE_MSIX) {
    pci_disable_msix(bfdev->pdev);
    kfree(bfdev->info.msix_entries);
    bfdev->info.msix_entries = NULL;
  }
  else if (bfdev->mode == BF_INTR_MODE_MSI)
    pci_disable_msi(bfdev->pdev);
fail_clear_pci_master:
  pci_clear_master(pdev);  
fail_release_iomem:
  bf_pci_release_iomem(&bfdev->info);
  pci_release_regions(pdev);
fail_pci_disable:
  pci_disable_device(pdev);
fail_free:
  kfree(bfdev);

  printk(KERN_ERR "bf probe not ok\n");
  return err;
}

static void
bf_pci_remove(struct pci_dev *pdev)
{
  struct bf_pci_dev *bfdev = pci_get_drvdata(pdev);

  bf_unregister_device(bfdev);
  if (bfdev->mode == BF_INTR_MODE_MSIX) {
    pci_disable_msix(pdev);
    kfree(bfdev->info.msix_entries);
    bfdev->info.msix_entries = NULL;
  }
  else if (bfdev->mode == BF_INTR_MODE_MSI)
    pci_disable_msi(pdev);
  pci_clear_master(pdev);
  bf_pci_release_iomem(&bfdev->info);
  pci_release_regions(pdev);
  pci_disable_pcie_error_reporting(pdev);
  pci_disable_device(pdev);
  pci_set_drvdata(pdev, NULL);
  kfree(bfdev);
}

/**
 * bf_pci_error_detected - called when PCI error is detected
 * @pdev: Pointer to PCI device
 * @state: The current pci connection state
 *
 * called when root complex detects pci error associated with the device
 */
static pci_ers_result_t bf_pci_error_detected(struct pci_dev *pdev,
                                              pci_channel_state_t state)
{
  struct bf_pci_dev *bfdev = pci_get_drvdata(pdev);
  int minor;

  if (!bfdev) {
    return PCI_ERS_RESULT_NONE;
  }
  printk(KERN_ERR "pci_err_detected state %d\n", state);
  if (state == pci_channel_io_perm_failure || state == pci_channel_io_frozen) {
    bfdev->info.pci_error_state = 1;
    /* send a signal to the user space program of the error */
    minor = bfdev->info.minor;
    if (minor < BF_MAX_DEVICE_CNT && bf_global[minor].async_queue) {
      kill_fasync(&bf_global[minor].async_queue, SIGIO, POLL_ERR);
    }
    return PCI_ERS_RESULT_DISCONNECT;
  } else {
    return PCI_ERS_RESULT_NONE;
  }
}

/**
 * bf_pci_slot_reset - called after the pci bus has been reset.
 * @pdev: Pointer to PCI device
 *
 * Restart the card from scratch, as if from a cold-boot.
 */
static pci_ers_result_t bf_pci_slot_reset(struct pci_dev *pdev)
{
  /* nothing to do for now as we do not expect to get backto normal after
   * a pcie link reset
   * TBD: fill in this function if tofino can recover after an error
   */
  return PCI_ERS_RESULT_DISCONNECT;
}

/**
 * bf_pci_resume - called when kernel thinks the device is up on PCIe.
 * @pdev: Pointer to PCI device
 *
 * This callback is called when the error recovery driver tells us that
 * its OK to resume normal operation.
 */
static void bf_pci_resume(struct pci_dev *pdev)
{
  /* this function should never be called  for Tofinoi */
  struct bf_pci_dev *bfdev = pci_get_drvdata(pdev);

  printk(KERN_ERR "BF io_resume invoked after pci error\n");
  if (bfdev) {
    bfdev->info.pci_error_state = 0;
  }
}

static int
bf_config_intr_mode(char *intr_str)
{
  if (!intr_str) {
    pr_info("Use MSIX interrupt by default\n");
    return 0;
  }

  if (!strcmp(intr_str, BF_INTR_MODE_MSIX_NAME)) {
    bf_intr_mode_default = BF_INTR_MODE_MSIX;
    pr_info("Use MSIX interrupt\n");
  } else if (!strcmp(intr_str, BF_INTR_MODE_MSI_NAME)) {
    bf_intr_mode_default = BF_INTR_MODE_MSI;
    pr_info("Use MSI interrupt\n");
  } else if (!strcmp(intr_str, BF_INTR_MODE_LEGACY_NAME)) {
    bf_intr_mode_default = BF_INTR_MODE_LEGACY;
    pr_info("Use legacy interrupt\n");
  } else {
    pr_info("Error: bad parameter - %s\n", intr_str);
    return -EINVAL;
  }

  return 0;
}

static const struct pci_device_id bf_pci_tbl[] = {
  {PCI_VDEVICE(BF, TOFINO_DEV_ID_A0), 0},
  {PCI_VDEVICE(BF, TOFINO_DEV_ID_B0), 0},
  /* required last entry */
  { .device = 0 }
};

/* PCI bus error handlers */
static struct pci_error_handlers bf_pci_err_handler = {
  .error_detected = bf_pci_error_detected,
  .slot_reset = bf_pci_slot_reset,
  .resume = bf_pci_resume,
};

static struct pci_driver bf_pci_driver = {
  .name = "bf",
  .id_table = bf_pci_tbl,
  .probe = bf_pci_probe,
  .remove = bf_pci_remove,
  .err_handler = &bf_pci_err_handler
};

static int __init
bfdrv_init(void)
{
  int ret;

  ret = bf_config_intr_mode(intr_mode);
  if (ret < 0)
    return ret;

  spin_lock_init(&bf_nonisr_lock);
  return pci_register_driver(&bf_pci_driver);
}

static void __exit
bfdrv_exit(void)
{
  pci_unregister_driver(&bf_pci_driver);
}

module_init(bfdrv_init);
module_exit(bfdrv_exit);

module_param(intr_mode, charp, S_IRUGO);
MODULE_PARM_DESC(intr_mode,
"bf interrupt mode (default=msix):\n"
"    " BF_INTR_MODE_MSIX_NAME "       Use MSIX interrupt\n"
"    " BF_INTR_MODE_MSI_NAME "        Use MSI interrupt\n"
"    " BF_INTR_MODE_LEGACY_NAME "     Use Legacy interrupt\n"
"\n");

MODULE_DEVICE_TABLE(pci, bf_pci_tbl);
MODULE_DESCRIPTION("Barefoot Tofino PCI device");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Barefoot Networks");
