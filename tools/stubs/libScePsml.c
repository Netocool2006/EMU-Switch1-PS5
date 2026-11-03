/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Link-time facade of the PS5 libScePsml module, library libScePsml_mfsr2 (PSSR 2): the nine
 * exports headless/pssr_bridge.cpp calls. The export names are not known, only their NIDs, so each
 * symbol is "__nid_" plus the NID with '+' written "_p" and '-' written "_m"; the native packaging
 * tool (nid() in sce_module_writer.cpp) turns that back into the NID. __sce_library_<name> names
 * the library the exports belong to. Nothing here is linked into the application or ever runs.
 */
#include <stdint.h>

int32_t __sce_library_libScePsml_mfsr2;

int32_t __nid_o_pNM86gEwFE(void) { return -1; }     /* initProfile                     */
int32_t __nid_1ic5q_mkdOsc(void) { return -1; }     /* getSharedResourcesInitRequirement */
int32_t __nid_gMduXCLYrNg(void) { return -1; }      /* createSharedResources           */
int32_t __nid_0GAw7SmkwII(void) { return -1; }      /* destroySharedResources          */
int32_t __nid_5z2gBlqxJ_p0(void) { return -1; }     /* getContextInitRequirement       */
int32_t __nid_m9JLPc3wOQw(void) { return -1; }      /* createContext                   */
int32_t __nid_Kyy1baXgaVU(void) { return -1; }      /* destroyContext                  */
int32_t __nid_ZLL31lzzxr4(void) { return -1; }      /* dispatch command size           */
int32_t __nid_lrJwpLjKXRc(void) { return -1; }      /* dispatch                        */
