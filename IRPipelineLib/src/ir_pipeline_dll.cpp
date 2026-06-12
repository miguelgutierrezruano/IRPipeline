/*
 * ir_pipeline_dll.cpp -- Thin DLL shim for the LWIR pipeline.
 *
 * This file does nothing but include the public header with IR_PIPELINE_BUILD
 * defined (done via preprocessor in the vcxproj), which causes all IR_API
 * functions to be __declspec(dllexport).
 *
 * The actual implementation lives in IRPipelineCore (ir_pipeline.cpp).
 * This project links IRPipelineCore.lib, so the exported symbols resolve
 * to the static library's object code.
 */

#include "ir_pipeline.h"
