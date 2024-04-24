DEFAULT_MAX_BP_ITERS = 30
DEFAULT_BP_METHOD = "minimum_sum"
DEFAULT_OSD_ORDER = 0
DEFAULT_OSD_METHOD = "osd_0"
DEFAULT_DECODINGS = 1
DEFAULT_WINDOW = 3
DEFAULT_COMMIT = 3
DEFAULT_BPOSD_DECODER_ARGS = {
    "max_iter": DEFAULT_MAX_BP_ITERS,
    "bp_method": DEFAULT_BP_METHOD,
    "osd_order": DEFAULT_OSD_ORDER,
    "osd_method": DEFAULT_OSD_METHOD,
}
DEFAULT_LSD_DECODER_ARGS = {
    "max_iter": DEFAULT_MAX_BP_ITERS,
    "bp_method": DEFAULT_BP_METHOD,
    "lsd_order": DEFAULT_OSD_ORDER,
    "lsd_method": DEFAULT_OSD_METHOD,
}