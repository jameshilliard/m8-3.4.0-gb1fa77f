#ifndef _PSYCHO_COMMON_H
#define _PSYCHO_COMMON_H

#define PSYCHO_CONFIG_BASE(PBM)	\
	((PBM)->config_space | (1UL << 24))
#define PSYCHO_CONFIG_ENCODE(BUS, DEVFN, REG)	\
	(((unsigned long)(BUS)   << 16) |	\
	 ((unsigned long)(DEVFN) << 8)  |	\
	 ((unsigned long)(REG)))

static inline void *psycho_pci_config_mkaddr(struct pci_pbm_info *pbm,
					     unsigned char bus,
					     unsigned int devfn,
					     int where)
{
	return (void *)
		(PSYCHO_CONFIG_BASE(pbm) |
		 PSYCHO_CONFIG_ENCODE(bus, devfn, where));
}

enum psycho_error_type {
	UE_ERR, CE_ERR, PCI_ERR
};

extern void psycho_check_iommu_error(struct pci_pbm_info *pbm,
				     unsigned long afsr,
				     unsigned long afar,
				     enum psycho_error_type type);

extern irqreturn_t psycho_pcierr_intr(int irq, void *dev_id);

extern int psycho_iommu_init(struct pci_pbm_info *pbm, int tsbsize,
			     u32 dvma_offset, u32 dma_mask,
			     unsigned long write_complete_offset);

extern void psycho_pbm_init_common(struct pci_pbm_info *pbm,
				   struct platform_device *op,
				   const char *chip_name, int chip_type);

#endif 
