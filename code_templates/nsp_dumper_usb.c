/*
 * main.c
 *
 * Copyright (c) 2020, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * nxdumptool is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "utils.h"
#include "gamecard.h"
#include "title.h"
#include "cnmt.h"
#include "program_info.h"
#include "nacp.h"
#include "legal_info.h"
#include "cert.h"
#include "usb.h"

#define BLOCK_SIZE  0x800000

static const char *dump_type_strings[] = {
    "dump base application",
    "dump update",
    "dump dlc"
};

static const u32 dump_type_strings_count = MAX_ELEMENTS(dump_type_strings);

typedef struct {
    char str[64];
    bool val;
} options_t;

static options_t options[] = {
    { "set download distribution type", false },
    { "remove console specific data", false },
    { "remove titlekey crypto (implies previous option)", false },
    { "change acid rsa key/sig", false }
};

static const u32 options_count = MAX_ELEMENTS(options);

static void consolePrint(const char *text, ...)
{
    va_list v;
    va_start(v, text);
    vfprintf(stdout, text, v);
    va_end(v);
    consoleUpdate(NULL);
}

static void nspDump(TitleInfo *title_info)
{
    if (!title_info || !title_info->content_count || !title_info->content_infos) return;
    
    consoleClear();
    
    TitleApplicationMetadata *app_metadata = (title_info->app_metadata ? title_info->app_metadata : ((title_info->parent && title_info->parent->app_metadata) ? title_info->parent->app_metadata : NULL));
    
    printf("%s info:\n\n", title_info->meta_key.type == NcmContentMetaType_Application ? "base application" : \
                           (title_info->meta_key.type == NcmContentMetaType_Patch ? "update" : "dlc"));
    printf("name: %s\n", app_metadata->lang_entry.name);
    printf("publisher: %s\n", app_metadata->lang_entry.author);
    printf("source storage: %s\n", title_info->storage_id == NcmStorageId_GameCard ? "gamecard" : (title_info->storage_id == NcmStorageId_BuiltInUser ? "emmc" : "sd card"));
    printf("title id: %016lX\n", title_info->meta_key.id);
    printf("version: %u (%u.%u.%u-%u.%u)\n", title_info->version.value, title_info->version.major, title_info->version.minor, title_info->version.micro, title_info->version.major_relstep, \
                                             title_info->version.minor_relstep);
    printf("content count: %u\n", title_info->content_count);
    printf("size: %s\n", title_info->size_str);
    printf("______________________________\n\n");
    printf("dump options:\n\n");
    for(u32 i = 0; i < options_count; i++) printf("%s: %s\n", options[i].str, options[i].val ? "yes" : "no");
    printf("______________________________\n\n");
    
    bool set_download_type = options[0].val, remove_console_data = options[1].val, remove_titlekey_crypto = options[2].val, change_acid_rsa = options[3].val;
    
    u8 *buf = NULL;
    char *dump_name = NULL, *path = NULL;
    
    NcaContext *nca_ctx = NULL;
    
    NcaContext *meta_nca_ctx = NULL;
    ContentMetaContext cnmt_ctx = {0};
    
    ProgramInfoContext *program_info_ctx = NULL;
    u32 program_idx = 0, program_count = titleGetContentCountByType(title_info, NcmContentType_Program);
    
    NacpContext *nacp_ctx = NULL;
    u32 control_idx = 0, control_count = titleGetContentCountByType(title_info, NcmContentType_Control);
    
    LegalInfoContext *legal_info_ctx = NULL;
    u32 legal_info_idx = 0, legal_info_count = titleGetContentCountByType(title_info, NcmContentType_LegalInformation);
    
    Ticket tik = {0};
    TikCommonBlock *tik_common_block = NULL;
    
    u8 *raw_cert_chain = NULL;
    u64 raw_cert_chain_size = 0;
    
    PartitionFileSystemFileContext pfs_file_ctx = {0};
    pfsInitializeFileContext(&pfs_file_ctx);
    
    char entry_name[64] = {0};
    u64 nsp_header_size = 0, nsp_size = 0, nsp_offset = 0;
    char *tmp_name = NULL;
    
    Sha256Context sha256_ctx = {0};
    u8 sha256_hash[SHA256_HASH_SIZE] = {0};
    
    /* Allocate memory for the dump process. */
    if (!(buf = usbAllocatePageAlignedBuffer(BLOCK_SIZE)))
    {
        consolePrint("buf alloc failed\n");
        goto end;
    }
    
    /* Generate output path. */
    if (!(dump_name = titleGenerateFileName(title_info, TitleFileNameConvention_Full, TitleFileNameIllegalCharReplaceType_IllegalFsChars)))
    {
        consolePrint("title generate file name failed\n");
        goto end;
    }
    
    if (!(path = utilsGeneratePath(NULL, dump_name, ".nsp")))
    {
        consolePrint("generate path failed\n");
        goto end;
    }
    
    if (!(nca_ctx = calloc(title_info->content_count, sizeof(NcaContext))))
    {
        consolePrint("nca ctx calloc failed\n");
        goto end;
    }
    
    if (program_count && !(program_info_ctx = calloc(program_count, sizeof(ProgramInfoContext))))
    {
        consolePrint("program info ctx calloc failed\n");
        goto end;
    }
    
    if (control_count && !(nacp_ctx = calloc(control_count, sizeof(NacpContext))))
    {
        consolePrint("nacp ctx calloc failed\n");
        goto end;
    }
    
    if (legal_info_count && !(legal_info_ctx = calloc(legal_info_count, sizeof(LegalInfoContext))))
    {
        consolePrint("legal info ctx calloc failed\n");
        goto end;
    }
    
    // set meta nca as the last nca
    meta_nca_ctx = &(nca_ctx[title_info->content_count - 1]);
    
    if (!ncaInitializeContext(meta_nca_ctx, title_info->storage_id, (title_info->storage_id == NcmStorageId_GameCard ? GameCardHashFileSystemPartitionType_Secure : 0), \
        titleGetContentInfoByTypeAndIdOffset(title_info, NcmContentType_Meta, 0), &tik))
    {
        consolePrint("Meta nca initialize ctx failed\n");
        goto end;
    }
    
    consolePrint("Meta nca initialize ctx succeeded\n");
    
    if (!cnmtInitializeContext(&cnmt_ctx, meta_nca_ctx))
    {
        consolePrint("cnmt initialize ctx failed\n");
        goto end;
    }
    
    consolePrint("cnmt initialize ctx succeeded (%s)\n", meta_nca_ctx->content_id_str);
    
    // initialize nca context
    // initialize content type context
    // generate nca patches (if needed)
    // generate content type xml
    for(u32 i = 0, j = 0; i < title_info->content_count; i++)
    {
        // skip meta nca since we already initialized it
        NcmContentInfo *content_info = &(title_info->content_infos[i]);
        if (content_info->content_type == NcmContentType_Meta) continue;
        
        NcaContext *cur_nca_ctx = &(nca_ctx[j]);
        if (!ncaInitializeContext(cur_nca_ctx, title_info->storage_id, (title_info->storage_id == NcmStorageId_GameCard ? GameCardHashFileSystemPartitionType_Secure : 0), content_info, &tik))
        {
            consolePrint("%s #%u initialize nca ctx failed\n", titleGetNcmContentTypeName(content_info->content_type), content_info->id_offset);
            goto end;
        }
        
        consolePrint("%s #%u initialize nca ctx succeeded\n", titleGetNcmContentTypeName(content_info->content_type), content_info->id_offset);
        
        // don't go any further with this nca if we can't access its fs data because it's pointless
        // to do: add preload warning
        if (cur_nca_ctx->rights_id_available && !cur_nca_ctx->titlekey_retrieved)
        {
            j++;
            continue;
        }
        
        // set download distribution type
        // has no effect if this nca uses NcaDistributionType_Download
        if (set_download_type) ncaSetDownloadDistributionType(cur_nca_ctx);
        
        // remove titlekey crypto
        // has no effect if this nca doesn't use titlekey crypto
        if (remove_titlekey_crypto && !ncaRemoveTitlekeyCrypto(cur_nca_ctx))
        {
            consolePrint("nca remove titlekey crypto failed\n");
            goto end;
        }
        
        switch(content_info->content_type)
        {
            case NcmContentType_Program:
            {
                ProgramInfoContext *cur_program_info_ctx = &(program_info_ctx[program_idx]);
                
                if (!programInfoInitializeContext(cur_program_info_ctx, cur_nca_ctx))
                {
                    consolePrint("initialize program info ctx failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                if (change_acid_rsa && !programInfoGenerateNcaPatch(cur_program_info_ctx))
                {
                    consolePrint("program info nca patch failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                if (!programInfoGenerateAuthoringToolXml(cur_program_info_ctx))
                {
                    consolePrint("program info xml failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                program_idx++;
                
                consolePrint("initialize program info ctx succeeded (%s)\n", cur_nca_ctx->content_id_str);
                
                break;
            }
            case NcmContentType_Control:
            {
                NacpContext *cur_nacp_ctx = &(nacp_ctx[control_idx]);
                
                if (!nacpInitializeContext(cur_nacp_ctx, cur_nca_ctx))
                {
                    consolePrint("initialize nacp ctx failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                // add nacp mods here
                
                if (!nacpGenerateAuthoringToolXml(cur_nacp_ctx, title_info->version.value, cnmtGetRequiredTitleVersion(&cnmt_ctx)))
                {
                    consolePrint("nacp xml failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                control_idx++;
                
                consolePrint("initialize nacp ctx succeeded (%s)\n", cur_nca_ctx->content_id_str);
                
                break;
            }
            case NcmContentType_LegalInformation:
            {
                LegalInfoContext *cur_legal_info_ctx = &(legal_info_ctx[legal_info_idx]);
                
                if (!legalInfoInitializeContext(cur_legal_info_ctx, cur_nca_ctx))
                {
                    consolePrint("initialize legal info ctx failed (%s)\n", cur_nca_ctx->content_id_str);
                    goto end;
                }
                
                legal_info_idx++;
                
                consolePrint("initialize legal info ctx succeeded (%s)\n", cur_nca_ctx->content_id_str);
                
                break;
            }
            default:
                break;
        }
        
        if (!ncaEncryptHeader(cur_nca_ctx))
        {
            consolePrint("%s #%u encrypt nca header failed\n", titleGetNcmContentTypeName(content_info->content_type), content_info->id_offset);
            goto end;
        }
        
        j++;
    }
    
    // generate cnmt xml right away even though we don't yet have all the data we need
    // This is because we need its size to calculate the full nsp size
    if (!cnmtGenerateAuthoringToolXml(&cnmt_ctx, nca_ctx, title_info->content_count))
    {
        consolePrint("cnmt xml #1 failed\n");
        goto end;
    }
    
    bool retrieve_tik_cert = (!remove_titlekey_crypto && tik.size > 0);
    if (retrieve_tik_cert)
    {
        if (!(tik_common_block = tikGetCommonBlock(tik.data)))
        {
            consolePrint("tik common block failed");
            goto end;
        }
        
        if (remove_console_data && tik_common_block->titlekey_type == TikTitleKeyType_Personalized)
        {
            if (!tikConvertPersonalizedTicketToCommonTicket(&tik, &raw_cert_chain, &raw_cert_chain_size))
            {
                consolePrint("tik convert failed\n");
                goto end;
            }
        } else {
            raw_cert_chain = (title_info->storage_id == NcmStorageId_GameCard ? certRetrieveRawCertificateChainFromGameCardByRightsId(&(tik_common_block->rights_id), &raw_cert_chain_size) : \
                                                                                certGenerateRawCertificateChainBySignatureIssuer(tik_common_block->issuer, &raw_cert_chain_size));
            if (!raw_cert_chain)
            {
                consolePrint("cert failed\n");
                goto end;
            }
        }
    }
    
    // add nca info
    for(u32 i = 0; i < title_info->content_count; i++)
    {
        NcaContext *cur_nca_ctx = &(nca_ctx[i]);
        sprintf(entry_name, "%s.%s", cur_nca_ctx->content_id_str, cur_nca_ctx->content_type == NcmContentType_Meta ? "cnmt.nca" : "nca");
        
        if (!pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, cur_nca_ctx->content_size, NULL))
        {
            consolePrint("pfs add entry failed: %s\n", entry_name);
            goto end;
        }
    }
    
    // add cnmt xml info
    sprintf(entry_name, "%s.cnmt.xml", meta_nca_ctx->content_id_str);
    if (!pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, cnmt_ctx.authoring_tool_xml_size, &(meta_nca_ctx->content_type_ctx_data_idx)))
    {
        consolePrint("pfs add entry failed: %s\n", entry_name);
        goto end;
    }
    
    // add content type ctx data info
    for(u32 i = 0; i < (title_info->content_count - 1); i++)
    {
        bool ret = false;
        NcaContext *cur_nca_ctx = &(nca_ctx[i]);
        if (!cur_nca_ctx->content_type_ctx) continue;
        
        switch(cur_nca_ctx->content_type)
        {
            case NcmContentType_Program:
            {
                ProgramInfoContext *cur_program_info_ctx = (ProgramInfoContext*)cur_nca_ctx->content_type_ctx;
                sprintf(entry_name, "%s.programinfo.xml", cur_nca_ctx->content_id_str);
                ret = pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, cur_program_info_ctx->authoring_tool_xml_size, &(cur_nca_ctx->content_type_ctx_data_idx));
                break;
            }
            case NcmContentType_Control:
            {
                NacpContext *cur_nacp_ctx = (NacpContext*)cur_nca_ctx->content_type_ctx;
                
                for(u8 j = 0; j < cur_nacp_ctx->icon_count; j++)
                {
                    NacpIconContext *icon_ctx = &(cur_nacp_ctx->icon_ctx[j]);
                    sprintf(entry_name, "%s.nx.%s.jpg", cur_nca_ctx->content_id_str, nacpGetLanguageString(icon_ctx->language));
                    if (!pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, icon_ctx->icon_size, j == 0 ? &(cur_nca_ctx->content_type_ctx_data_idx) : NULL))
                    {
                        consolePrint("pfs add entry failed: %s\n", entry_name);
                        goto end;
                    }
                }
                
                sprintf(entry_name, "%s.nacp.xml", cur_nca_ctx->content_id_str);
                ret = pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, cur_nacp_ctx->authoring_tool_xml_size, NULL);
                break;
            }
            case NcmContentType_LegalInformation:
            {
                LegalInfoContext *cur_legal_info_ctx = (LegalInfoContext*)cur_nca_ctx->content_type_ctx;
                sprintf(entry_name, "%s.legalinfo.xml", cur_nca_ctx->content_id_str);
                ret = pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, cur_legal_info_ctx->authoring_tool_xml_size, &(cur_nca_ctx->content_type_ctx_data_idx));
                break;
            }
            default:
                break;
        }
        
        if (!ret)
        {
            consolePrint("pfs add entry failed: %s\n", entry_name);
            goto end;
        }
    }
    
    // add ticket and cert info
    if (retrieve_tik_cert)
    {
        sprintf(entry_name, "%s.tik", tik.rights_id_str);
        if (!pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, tik.size, NULL))
        {
            consolePrint("pfs add entry failed: %s\n", entry_name);
            goto end;
        }
        
        sprintf(entry_name, "%s.cert", tik.rights_id_str);
        if (!pfsAddEntryInformationToFileContext(&pfs_file_ctx, entry_name, raw_cert_chain_size, NULL))
        {
            consolePrint("pfs add entry failed: %s\n", entry_name);
            goto end;
        }
    }
    
    // write buffer to memory buffer
    if (!pfsWriteFileContextHeaderToMemoryBuffer(&pfs_file_ctx, buf, BLOCK_SIZE, &nsp_header_size))
    {
        consolePrint("pfs write header to mem #1 failed\n");
        goto end;
    }
    
    nsp_size = (nsp_header_size + pfs_file_ctx.fs_size);
    consolePrint("nsp header size: 0x%lX | nsp size: 0x%lX\n", nsp_header_size, nsp_size);
    
    consolePrint("waiting for usb connection... ");
    
    time_t start = time(NULL);
    bool usb_conn = false;
    
    while(true)
    {
        time_t now = time(NULL);
        if ((now - start) >= 10) break;
        consolePrint("%lu ", now - start);
        
        if ((usb_conn = usbIsReady())) break;
        utilsSleep(1);
    }
    
    consolePrint("\n");
    
    if (!usb_conn)
    {
        consolePrint("usb connection failed\n");
        goto end;
    }
    
    consolePrint("dump process started. please wait...\n");
    
    start = time(NULL);
    
    if (!usbSendFileProperties(nsp_size, path, (u32)nsp_header_size))
    {
        consolePrint("usb send file properties (header) failed\n");
        goto end;
    }
    
    nsp_offset += nsp_header_size;
    
    // write ncas
    for(u32 i = 0; i < title_info->content_count; i++)
    {
        NcaContext *cur_nca_ctx = &(nca_ctx[i]);
        u64 blksize = BLOCK_SIZE;
        
        memset(&sha256_ctx, 0, sizeof(Sha256Context));
        sha256ContextCreate(&sha256_ctx);
        
        if (cur_nca_ctx->content_type == NcmContentType_Meta && (!cnmtGenerateNcaPatch(&cnmt_ctx) || !ncaEncryptHeader(cur_nca_ctx)))
        {
            consolePrint("cnmt generate patch failed\n");
            goto end;
        }
        
        bool dirty_header = ncaIsHeaderDirty(cur_nca_ctx);
        
        tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, i);
        if (!usbSendFilePropertiesCommon(cur_nca_ctx->content_size, tmp_name))
        {
            consolePrint("usb send file properties \"%s\" failed\n", tmp_name);
            goto end;
        }
        
        for(u64 offset = 0; offset < cur_nca_ctx->content_size; offset += blksize, nsp_offset += blksize)
        {
            if ((cur_nca_ctx->content_size - offset) < blksize) blksize = (cur_nca_ctx->content_size - offset);
            
            // read nca chunk
            if (!ncaReadContentFile(cur_nca_ctx, buf, blksize, offset))
            {
                consolePrint("nca read failed at 0x%lX for \"%s\"\n", offset, cur_nca_ctx->content_id_str);
                goto end;
            }
            
            if (dirty_header)
            {
                // write re-encrypted headers
                if (!cur_nca_ctx->header_written) ncaWriteEncryptedHeaderDataToMemoryBuffer(cur_nca_ctx, buf, blksize, offset);
                
                if (cur_nca_ctx->content_type_ctx_patch)
                {
                    // write content type context patch
                    switch(cur_nca_ctx->content_type)
                    {
                        case NcmContentType_Meta:
                        {
                            cnmtWriteNcaPatch(&cnmt_ctx, buf, blksize, offset);
                            break;
                        }
                        case NcmContentType_Program:
                        {
                            ProgramInfoContext *cur_program_info_ctx = (ProgramInfoContext*)cur_nca_ctx->content_type_ctx;
                            programInfoWriteNcaPatch(cur_program_info_ctx, buf, blksize, offset);
                            break;
                        }
                        case NcmContentType_Control:
                            // write nacp patches here
                            break;
                        default:
                            break;
                    }
                }
                
                // update flag to avoid entering this code block if it's not needed anymore
                dirty_header = (!cur_nca_ctx->header_written || cur_nca_ctx->content_type_ctx_patch);
            }
            
            // update hash calculation
            sha256ContextUpdate(&sha256_ctx, buf, blksize);
            
            // write nca chunk
            if (!usbSendFileData(buf, blksize))
            {
                consolePrint("send file data failed\n");
                goto end;
            }
        }
        
        // get hash
        sha256ContextGetHash(&sha256_ctx, sha256_hash);
        
        // update content id and hash
        ncaUpdateContentIdAndHash(cur_nca_ctx, sha256_hash);
        
        // update cnmt
        if (!cnmtUpdateContentInfo(&cnmt_ctx, cur_nca_ctx))
        {
            consolePrint("cnmt update content info failed\n");
            goto end;
        }
        
        // update pfs entry name
        if (!pfsUpdateEntryNameFromFileContext(&pfs_file_ctx, i, cur_nca_ctx->content_id_str))
        {
            consolePrint("pfs update entry name failed for nca \"%s\"\n", cur_nca_ctx->content_id_str);
            goto end;
        }
    }
    
    // regenerate cnmt xml
    if (!cnmtGenerateAuthoringToolXml(&cnmt_ctx, nca_ctx, title_info->content_count))
    {
        consolePrint("cnmt xml #2 failed\n");
        goto end;
    }
    
    // write cnmt xml
    tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, meta_nca_ctx->content_type_ctx_data_idx);
    if (!usbSendFilePropertiesCommon(cnmt_ctx.authoring_tool_xml_size, tmp_name) || !usbSendFileData(cnmt_ctx.authoring_tool_xml, cnmt_ctx.authoring_tool_xml_size))
    {
        consolePrint("send \"%s\" failed\n", tmp_name);
        goto end;
    }
    
    nsp_offset += cnmt_ctx.authoring_tool_xml_size;
    
    // update cnmt xml pfs entry name
    if (!pfsUpdateEntryNameFromFileContext(&pfs_file_ctx, meta_nca_ctx->content_type_ctx_data_idx, meta_nca_ctx->content_id_str))
    {
        consolePrint("pfs update entry name cnmt xml failed\n");
        goto end;
    }
    
    // write content type ctx data
    for(u32 i = 0; i < (title_info->content_count - 1); i++)
    {
        NcaContext *cur_nca_ctx = &(nca_ctx[i]);
        if (!cur_nca_ctx->content_type_ctx) continue;
        
        char *authoring_tool_xml = NULL;
        u64 authoring_tool_xml_size = 0;
        u32 data_idx = cur_nca_ctx->content_type_ctx_data_idx;
        
        switch(cur_nca_ctx->content_type)
        {
            case NcmContentType_Program:
            {
                ProgramInfoContext *cur_program_info_ctx = (ProgramInfoContext*)cur_nca_ctx->content_type_ctx;
                authoring_tool_xml = cur_program_info_ctx->authoring_tool_xml;
                authoring_tool_xml_size = cur_program_info_ctx->authoring_tool_xml_size;
                break;
            }
            case NcmContentType_Control:
            {
                NacpContext *cur_nacp_ctx = (NacpContext*)cur_nca_ctx->content_type_ctx;
                authoring_tool_xml = cur_nacp_ctx->authoring_tool_xml;
                authoring_tool_xml_size = cur_nacp_ctx->authoring_tool_xml_size;
                
                // loop through available icons
                for(u8 j = 0; j < cur_nacp_ctx->icon_count; j++)
                {
                    NacpIconContext *icon_ctx = &(cur_nacp_ctx->icon_ctx[j]);
                    
                    // write icon
                    tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, data_idx);
                    if (!usbSendFilePropertiesCommon(icon_ctx->icon_size, tmp_name) || !usbSendFileData(icon_ctx->icon_data, icon_ctx->icon_size))
                    {
                        consolePrint("send \"%s\" failed\n", tmp_name);
                        goto end;
                    }
                    
                    nsp_offset += icon_ctx->icon_size;
                    
                    // update pfs entry name
                    if (!pfsUpdateEntryNameFromFileContext(&pfs_file_ctx, data_idx++, cur_nca_ctx->content_id_str))
                    {
                        consolePrint("pfs update entry name failed for icon \"%s\" (%u)\n", cur_nca_ctx->content_id_str, icon_ctx->language);
                        goto end;
                    }
                }
                
                break;
            }
            case NcmContentType_LegalInformation:
            {
                LegalInfoContext *cur_legal_info_ctx = (LegalInfoContext*)cur_nca_ctx->content_type_ctx;
                authoring_tool_xml = cur_legal_info_ctx->authoring_tool_xml;
                authoring_tool_xml_size = cur_legal_info_ctx->authoring_tool_xml_size;
                break;
            }
            default:
                break;
        }
        
        // write xml
        tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, data_idx);
        if (!usbSendFilePropertiesCommon(authoring_tool_xml_size, tmp_name) || !usbSendFileData(authoring_tool_xml, authoring_tool_xml_size))
        {
            consolePrint("send \"%s\" failed\n", tmp_name);
            goto end;
        }
        
        nsp_offset += authoring_tool_xml_size;
        
        // update pfs entry name
        if (!pfsUpdateEntryNameFromFileContext(&pfs_file_ctx, data_idx, cur_nca_ctx->content_id_str))
        {
            consolePrint("pfs update entry name failed for xml \"%s\"\n", cur_nca_ctx->content_id_str);
            goto end;
        }
    }
    
    if (retrieve_tik_cert)
    {
        // write ticket
        tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, pfs_file_ctx.header.entry_count - 2);
        if (!usbSendFilePropertiesCommon(tik.size, tmp_name) || !usbSendFileData(tik.data, tik.size))
        {
            consolePrint("send \"%s\" failed\n", tmp_name);
            goto end;
        }
        
        nsp_offset += tik.size;
        
        // write cert
        tmp_name = pfsGetEntryNameByIndexFromFileContext(&pfs_file_ctx, pfs_file_ctx.header.entry_count - 1);
        if (!usbSendFilePropertiesCommon(raw_cert_chain_size, tmp_name) || !usbSendFileData(raw_cert_chain, raw_cert_chain_size))
        {
            consolePrint("send \"%s\" failed\n", tmp_name);
            goto end;
        }
        
        nsp_offset += raw_cert_chain_size;
    }
    
    // write new pfs0 header
    if (!pfsWriteFileContextHeaderToMemoryBuffer(&pfs_file_ctx, buf, BLOCK_SIZE, &nsp_header_size))
    {
        consolePrint("pfs write header to mem #2 failed\n");
        goto end;
    }
    
    if (!usbSendNspHeader(buf, (u32)nsp_header_size))
    {
        consolePrint("send nsp header failed\n");
        goto end;
    }
    
    start = (time(NULL) - start);
    consolePrint("process successfully completed in %lu seconds!\n", start);
    
end:
    pfsFreeFileContext(&pfs_file_ctx);
    
    if (raw_cert_chain) free(raw_cert_chain);
    
    if (legal_info_ctx)
    {
        for(u32 i = 0; i < legal_info_count; i++) legalInfoFreeContext(&(legal_info_ctx[i]));
        free(legal_info_ctx);
    }
    
    if (nacp_ctx)
    {
        for(u32 i = 0; i < control_count; i++) nacpFreeContext(&(nacp_ctx[i]));
        free(nacp_ctx);
    }
    
    if (program_info_ctx)
    {
        for(u32 i = 0; i < program_count; i++) programInfoFreeContext(&(program_info_ctx[i]));
        free(program_info_ctx);
    }
    
    cnmtFreeContext(&cnmt_ctx);
    
    if (nca_ctx) free(nca_ctx);
    
    if (path) free(path);
    
    if (dump_name) free(dump_name);
    
    if (buf) free(buf);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    
    int ret = 0;
    
    consoleInit(NULL);
    
    consolePrint("initializing...\n");
    
    if (!utilsInitializeResources())
    {
        ret = -1;
        goto out;
    }
    
    u32 app_count = 0;
    TitleApplicationMetadata **app_metadata = NULL;
    TitleUserApplicationData user_app_data = {0};
    TitleInfo *title_info = NULL;
    
    u32 menu = 0, selected_idx = 0, scroll = 0, page_size = 30;
    
    u32 title_idx = 0, title_scroll = 0;
    u32 type_idx = 0, type_scroll = 0;
    u32 list_count = 0, list_idx = 0;
    
    app_metadata = titleGetApplicationMetadataEntries(false, &app_count);
    if (!app_metadata || !app_count)
    {
        consolePrint("app metadata failed\n");
        goto out2;
    }
    
    consolePrint("app metadata succeeded\n");
    
    utilsSleep(1);
    
    while(true)
    {
        consoleClear();
        
        printf("press b to %s.\n", menu == 0 ? "exit" : "go back");
        printf("______________________________\n\n");
        
        if (menu == 0)
        {
            printf("title: %u / %u\n", selected_idx + 1, app_count);
            printf("selected title: %016lX - %s\n", app_metadata[selected_idx]->title_id, app_metadata[selected_idx]->lang_entry.name);
        } else {
            printf("title info:\n\n");
            printf("name: %s\n", app_metadata[title_idx]->lang_entry.name);
            printf("publisher: %s\n", app_metadata[title_idx]->lang_entry.author);
            printf("title id: %016lX\n", app_metadata[title_idx]->title_id);
            
            if (menu == 2)
            {
                printf("______________________________\n\n");
                
                if (title_info->previous || title_info->next)
                {
                    printf("press zl/l and/or zr/r to change the selected title\n");
                    printf("title: %u / %u\n", list_idx, list_count);
                    printf("______________________________\n\n");
                }
                
                printf("selected %s info:\n\n", title_info->meta_key.type == NcmContentMetaType_Application ? "base application" : \
                                                (title_info->meta_key.type == NcmContentMetaType_Patch ? "update" : "dlc"));
                printf("source storage: %s\n", title_info->storage_id == NcmStorageId_GameCard ? "gamecard" : (title_info->storage_id == NcmStorageId_BuiltInUser ? "emmc" : "sd card"));
                if (title_info->meta_key.type != NcmContentMetaType_Application) printf("title id: %016lX\n", title_info->meta_key.id);
                printf("version: %u (%u.%u.%u-%u.%u)\n", title_info->version.value, title_info->version.major, title_info->version.minor, title_info->version.micro, title_info->version.major_relstep, \
                                                         title_info->version.minor_relstep);
                printf("content count: %u\n", title_info->content_count);
                printf("size: %s\n", title_info->size_str);
            }
        }
        
        printf("______________________________\n\n");
        
        u32 max_val = (menu == 0 ? app_count : (menu == 1 ? dump_type_strings_count : (1 + options_count)));
        for(u32 i = scroll; i < max_val; i++)
        {
            if (i >= (scroll + page_size)) break;
            
            printf("%s", i == selected_idx ? " -> " : "    ");
            
            if (menu == 0)
            {
                printf("%016lX - %s\n", app_metadata[i]->title_id, app_metadata[i]->lang_entry.name);
            } else
            if (menu == 1)
            {
                printf("%s\n", dump_type_strings[i]);
            } else
            if (menu == 2)
            {
                if (i == 0)
                {
                    printf("start nsp dump\n");
                } else {
                    printf("%s: < %s >\n", options[i - 1].str, options[i - 1].val ? "yes" : "no");
                }
            }
        }
        
        printf("\n");
        
        consoleUpdate(NULL);
        
        bool gc_update = false;
        u64 btn_down = 0, btn_held = 0;
        
        while(true)
        {
            hidScanInput();
            btn_down = utilsHidKeysAllDown();
            btn_held = utilsHidKeysAllHeld();
            if (btn_down || btn_held) break;
            
            if (titleIsGameCardInfoUpdated())
            {
                free(app_metadata);
                
                app_metadata = titleGetApplicationMetadataEntries(false, &app_count);
                if (!app_metadata)
                {
                    consolePrint("\napp metadata failed\n");
                    goto out2;
                }
                
                menu = selected_idx = scroll = 0;
                
                title_idx = title_scroll = 0;
                type_idx = type_scroll = 0;
                list_count = list_idx = 0;
                
                gc_update = true;
                
                break;
            }
        }
        
        if (gc_update) continue;
        
        if (btn_down & KEY_A)
        {
            bool error = false;
            
            if (menu == 0)
            {
                title_idx = selected_idx;
                title_scroll = scroll;
            } else
            if (menu == 1)
            {
                type_idx = selected_idx;
                type_scroll = scroll;
            }
            
            menu++;
            
            if (menu == 3 && selected_idx != 0)
            {
                menu--;
                continue;
            }
            
            if (menu == 1)
            {
                if (!titleGetUserApplicationData(app_metadata[title_idx]->title_id, &user_app_data))
                {
                    consolePrint("\nget user application data failed!\n");
                    error = true;
                }
            } else
            if (menu == 2)
            {
                if ((type_idx == 0 && !user_app_data.app_info) || (type_idx == 1 && !user_app_data.patch_info) || (type_idx == 2 && !user_app_data.aoc_info))
                {
                    consolePrint("\nthe selected title doesn't have available %s data\n", type_idx == 0 ? "base application" : (type_idx == 1 ? "update" : "dlc"));
                    error = true;
                } else {
                    title_info = (type_idx == 0 ? user_app_data.app_info : (type_idx == 1 ? user_app_data.patch_info : user_app_data.aoc_info));
                    list_count = titleGetCountFromInfoBlock(title_info);
                    list_idx = 1;
                }
            } else
            if (menu == 3)
            {
                consoleClear();
                utilsChangeHomeButtonBlockStatus(true);
                nspDump(title_info);
                utilsChangeHomeButtonBlockStatus(false);
            }
            
            if (error || menu >= 3)
            {
                consolePrint("press any button to continue\n");
                utilsWaitForButtonPress(KEY_ANY);
                menu--;
            } else {
                selected_idx = scroll = 0;
            }
        } else
        if ((btn_down & KEY_DDOWN) || (btn_held & (KEY_LSTICK_DOWN | KEY_RSTICK_DOWN)))
        {
            selected_idx++;
            
            if (selected_idx >= max_val)
            {
                if (btn_down & KEY_DDOWN)
                {
                    selected_idx = scroll = 0;
                } else {
                    selected_idx = (max_val - 1);
                }
            } else
            if (selected_idx >= (scroll + (page_size / 2)) && max_val > (scroll + page_size))
            {
                scroll++;
            }
        } else
        if ((btn_down & KEY_DUP) || (btn_held & (KEY_LSTICK_UP | KEY_RSTICK_UP)))
        {
            selected_idx--;
            
            if (selected_idx == UINT32_MAX)
            {
                if (btn_down & KEY_DUP)
                {
                    selected_idx = (max_val - 1);
                    scroll = (max_val >= page_size ? (max_val - page_size) : 0);
                } else {
                    selected_idx = 0;
                }
            } else
            if (selected_idx < (scroll + (page_size / 2)) && scroll > 0)
            {
                scroll--;
            }
        } else
        if (btn_down & KEY_B)
        {
            menu--;
            
            if (menu == UINT32_MAX)
            {
                break;
            } else {
                selected_idx = (menu == 0 ? title_idx : type_idx);
                scroll = (menu == 0 ? title_scroll : type_scroll);
            }
        } else
        if (((btn_down & KEY_DLEFT) || (btn_down & KEY_DRIGHT)) && menu == 2 && selected_idx != 0)
        {
            options[selected_idx - 1].val ^= 1;
        } else
        if (((btn_down & KEY_L) || (btn_down & KEY_ZL)) && menu == 2 && title_info->previous)
        {
            title_info = title_info->previous;
            list_idx--;
        } else
        if (((btn_down & KEY_R) || (btn_down & KEY_ZR)) && menu == 2 && title_info->next)
        {
            title_info = title_info->next;
            list_idx++;
        }
        
        if (btn_held & (KEY_LSTICK_DOWN | KEY_RSTICK_DOWN | KEY_LSTICK_UP | KEY_RSTICK_UP)) svcSleepThread(50000000); // 50 ms
    }
    
out2:
    if (menu != UINT32_MAX)
    {
        consolePrint("press any button to exit\n");
        utilsWaitForButtonPress(KEY_ANY);
    }
    
    if (app_metadata) free(app_metadata);
    
out:
    utilsCloseResources();
    
    consoleExit(NULL);
    
    return ret;
}
