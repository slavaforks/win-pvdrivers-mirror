/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2013 James Harper

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#if defined(__x86_64__)
  #define LongLongToPtr(x) (PVOID)(x)
#else
  #define LongLongToPtr(x) UlongToPtr(x)
#endif

#if defined(__x86_64__)
  #define ABI_PROTOCOL "x86_64-abi"
#else
  #define ABI_PROTOCOL "x86_32-abi"
#endif

ULONGLONG parse_numeric_string(PCHAR string) {
  ULONGLONG val = 0;
  while (*string != 0) {
    val = val * 10 + (*string - '0');
    string++;
  }
  return val;
}

/* called with StartIoLock held */
static blkif_shadow_t *
get_shadow_from_freelist(PXENVBD_DEVICE_DATA xvdd)
{
  if (xvdd->shadow_free == 0) {
    KdPrint((__DRIVER_NAME "     No more shadow entries\n"));
    return NULL;
  }
  xvdd->shadow_free--;
  if (xvdd->shadow_free < xvdd->shadow_min_free)
    xvdd->shadow_min_free = xvdd->shadow_free;
  return &xvdd->shadows[xvdd->shadow_free_list[xvdd->shadow_free]];
}

/* called with StartIoLock held */
static VOID
put_shadow_on_freelist(PXENVBD_DEVICE_DATA xvdd, blkif_shadow_t *shadow)
{
  xvdd->shadow_free_list[xvdd->shadow_free] = (USHORT)(shadow->req.id & SHADOW_ID_ID_MASK);
  shadow->srb = NULL;
  shadow->reset = FALSE;
  shadow->aligned_buffer_in_use = FALSE;
  xvdd->shadow_free++;
}

static __inline ULONG
decode_cdb_length(PSCSI_REQUEST_BLOCK srb)
{
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_WRITE:
    return ((ULONG)(UCHAR)srb->Cdb[7] << 8) | (ULONG)(UCHAR)srb->Cdb[8];
  case SCSIOP_READ16:
  case SCSIOP_WRITE16:
    return ((ULONG)(UCHAR)srb->Cdb[10] << 24) | ((ULONG)(UCHAR)srb->Cdb[11] << 16) | ((ULONG)(UCHAR)srb->Cdb[12] << 8) | (ULONG)(UCHAR)srb->Cdb[13];    
  default:
    return 0;
  }
}

static blkif_response_t *
XenVbd_GetResponse(PXENVBD_DEVICE_DATA xvdd, int i) {
  return RING_GET_RESPONSE(&xvdd->ring, i);
}

static VOID
XenVbd_PutRequest(PXENVBD_DEVICE_DATA xvdd, blkif_request_t *req) {
  *RING_GET_REQUEST(&xvdd->ring, xvdd->ring.req_prod_pvt) = *req;
  xvdd->ring.req_prod_pvt++;
}

static VOID
XenVbd_PutSrbOnList(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb) {
  srb_list_entry_t *srb_entry = srb->SrbExtension;
  srb_entry->srb = srb;
  srb_entry->outstanding_requests = 0;
  srb_entry->length = srb->DataTransferLength;
  srb_entry->offset = 0;
  srb_entry->error = FALSE;
  InsertTailList(&xvdd->srb_list, (PLIST_ENTRY)srb_entry);
}

static __inline ULONGLONG
decode_cdb_sector(PSCSI_REQUEST_BLOCK srb)
{
  ULONGLONG sector;
  
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_WRITE:
    sector = ((ULONG)(UCHAR)srb->Cdb[2] << 24) | ((ULONG)(UCHAR)srb->Cdb[3] << 16) | ((ULONG)(UCHAR)srb->Cdb[4] << 8) | (ULONG)(UCHAR)srb->Cdb[5];
    break;
  case SCSIOP_READ16:
  case SCSIOP_WRITE16:
    sector = ((ULONGLONG)(UCHAR)srb->Cdb[2] << 56) | ((ULONGLONG)(UCHAR)srb->Cdb[3] << 48)
           | ((ULONGLONG)(UCHAR)srb->Cdb[4] << 40) | ((ULONGLONG)(UCHAR)srb->Cdb[5] << 32)
           | ((ULONGLONG)(UCHAR)srb->Cdb[6] << 24) | ((ULONGLONG)(UCHAR)srb->Cdb[7] << 16)
           | ((ULONGLONG)(UCHAR)srb->Cdb[8] << 8) | ((ULONGLONG)(UCHAR)srb->Cdb[9]);
    //KdPrint((__DRIVER_NAME "     sector_number = %d (high) %d (low)\n", (ULONG)(sector >> 32), (ULONG)sector));
    break;
  default:
    sector = 0;
    break;
  }
  return sector;
}

static __inline BOOLEAN
decode_cdb_is_read(PSCSI_REQUEST_BLOCK srb)
{
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_READ16:
    return TRUE;
  case SCSIOP_WRITE:
  case SCSIOP_WRITE16:
    return FALSE;
  default:
    return FALSE;
  }
}

static ULONG
XenVbd_MakeSense(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb, UCHAR sense_key, UCHAR additional_sense_code)
{
  PSENSE_DATA sd = srb->SenseInfoBuffer;
 
  UNREFERENCED_PARAMETER(xvdd);
  
  if (!srb->SenseInfoBuffer)
    return 0;
  
  sd->ErrorCode = 0x70;
  sd->Valid = 1;
  sd->SenseKey = sense_key;
  sd->AdditionalSenseLength = sizeof(SENSE_DATA) - FIELD_OFFSET(SENSE_DATA, AdditionalSenseLength);
  sd->AdditionalSenseCode = additional_sense_code;
  return sizeof(SENSE_DATA);
}

static VOID
XenVbd_MakeAutoSense(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  if (srb->SrbStatus == SRB_STATUS_SUCCESS || srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)
    return;
  XenVbd_MakeSense(xvdd, srb, xvdd->last_sense_key, xvdd->last_additional_sense_code);
  srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
}

/* called with StartIo lock held */
static VOID
XenVbd_HandleEvent(PXENVBD_DEVICE_DATA xvdd)
{
  PSCSI_REQUEST_BLOCK srb;
  RING_IDX i, rp;
  ULONG j;
  blkif_response_t *rep;
  //int block_count;
  int more_to_do = TRUE;
  blkif_shadow_t *shadow;
  srb_list_entry_t *srb_entry;

  //if (dump_mode)
  //FUNCTION_ENTER();

  while (more_to_do) {
    rp = xvdd->ring.sring->rsp_prod;
    KeMemoryBarrier();
    for (i = xvdd->ring.rsp_cons; i != rp; i++) {
      rep = XenVbd_GetResponse(xvdd, i);
      shadow = &xvdd->shadows[rep->id & SHADOW_ID_ID_MASK];
      if (shadow->reset) {
        KdPrint((__DRIVER_NAME "     discarding reset shadow\n"));
        for (j = 0; j < shadow->req.nr_segments; j++) {
          XnEndAccess(xvdd->handle,
            shadow->req.seg[j].gref, FALSE, xvdd->grant_tag);
        }
      } else if (dump_mode && !(rep->id & SHADOW_ID_DUMP_FLAG)) {
        KdPrint((__DRIVER_NAME "     discarding stale (non-dump-mode) shadow\n"));
      } else {
        srb = shadow->srb;
        NT_ASSERT(srb);
        srb_entry = srb->SrbExtension;
        NT_ASSERT(srb_entry);
        /* a few errors occur in dump mode because Xen refuses to allow us to map pages we are using for other stuff. Just ignore them */
        if (rep->status == BLKIF_RSP_OKAY || (dump_mode &&  dump_mode_errors++ < DUMP_MODE_ERROR_LIMIT)) {
          srb->SrbStatus = SRB_STATUS_SUCCESS;
        } else {
          KdPrint((__DRIVER_NAME "     Xen Operation returned error\n"));
          if (decode_cdb_is_read(srb))
            KdPrint((__DRIVER_NAME "     Operation = Read\n"));
          else
            KdPrint((__DRIVER_NAME "     Operation = Write\n"));
          srb_entry->error = TRUE;
        }
        if (shadow->aligned_buffer_in_use) {
          NT_ASSERT(xvdd->aligned_buffer_in_use);
          xvdd->aligned_buffer_in_use = FALSE;
          if (srb->SrbStatus == SRB_STATUS_SUCCESS && decode_cdb_is_read(srb))
            memcpy((PUCHAR)shadow->system_address, xvdd->aligned_buffer, shadow->length);
        }
        for (j = 0; j < shadow->req.nr_segments; j++) {
          XnEndAccess(xvdd->handle, shadow->req.seg[j].gref, FALSE, xvdd->grant_tag);
        }
        srb_entry->outstanding_requests--;
        if (!srb_entry->outstanding_requests && srb_entry->offset == srb_entry->length) {
          if (srb_entry->error) {
            srb->SrbStatus = SRB_STATUS_ERROR;
            srb->ScsiStatus = 0x02;
            xvdd->last_sense_key = SCSI_SENSE_MEDIUM_ERROR;
            xvdd->last_additional_sense_code = SCSI_ADSENSE_NO_SENSE;
            XenVbd_MakeAutoSense(xvdd, srb);
          }
          //FUNCTION_MSG("Completing Srb = %p\n", srb);
          SxxxPortNotification(RequestComplete, xvdd, srb);
          SxxxPortNotification(NextLuRequest, xvdd, 0, 0, 0);
        }
      }
      put_shadow_on_freelist(xvdd, shadow);
    }

    xvdd->ring.rsp_cons = i;
    if (i != xvdd->ring.req_prod_pvt) {
      RING_FINAL_CHECK_FOR_RESPONSES(&xvdd->ring, more_to_do);
    } else {
      xvdd->ring.sring->rsp_event = i + 1;
      more_to_do = FALSE;
    }
  }

  if (dump_mode || xvdd->device_state == DEVICE_STATE_ACTIVE) {
    XenVbd_ProcessSrbList(xvdd);
  } else if (xvdd->shadow_free == SHADOW_ENTRIES) {
    FUNCTION_MSG("ring now empty - scheduling workitem for disconnect\n");
    XenVbd_CompleteDisconnect(xvdd);
    //IoQueueWorkItem(xvdd->disconnect_workitem, XenVbd_DisconnectWorkItem, DelayedWorkQueue, xvdd);
  }
  //if (dump_mode)
  //FUNCTION_EXIT();
  return;
}

/* called with StartIoLock held */
/* returns TRUE if something was put on the ring and notify might be required */
static BOOLEAN
XenVbd_PutSrbOnRing(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  srb_list_entry_t *srb_entry = srb->SrbExtension;
  /* sector_number and block_count are the adjusted-to-512-byte-sector values */
  ULONGLONG sector_number;
  ULONG block_count;
  blkif_shadow_t *shadow;
  ULONG remaining, offset, length;
  grant_ref_t gref;
  PUCHAR ptr;
  int i;
  PVOID system_address;

  //if (dump_mode) FUNCTION_ENTER();
  //FUNCTION_MSG("Srb = %p\n", srb);

  //FUNCTION_MSG("aligned_buffer_in_use = %d\n", xvdd->aligned_buffer_in_use);
  //FUNCTION_MSG("shadow_free = %d\n", xvdd->shadow_free);
  
  NT_ASSERT(srb);

  if (!dump_mode) {
#if 0
    if (StorPortGetSystemAddress(xvdd, srb, &system_address) != STOR_STATUS_SUCCESS) {
      FUNCTION_MSG("Failed to map DataBuffer\n");
      InsertHeadList(&xvdd->srb_list, (PLIST_ENTRY)srb->SrbExtension);
      //if (dump_mode) FUNCTION_EXIT();
      return FALSE;
    }
    system_address = (PUCHAR)system_address + srb_entry->offset;
#endif
    system_address = (PUCHAR)srb->DataBuffer + srb_entry->offset;
  } else {
    //FUNCTION_MSG("DataBuffer = %p\n", srb->DataBuffer);
    system_address = (PUCHAR)srb->DataBuffer + srb_entry->offset;
  }
  block_count = decode_cdb_length(srb);
  sector_number = decode_cdb_sector(srb);
  block_count *= xvdd->bytes_per_sector / 512;
  sector_number *= xvdd->bytes_per_sector / 512;

  NT_ASSERT(block_count * 512 == srb->DataTransferLength);
  
  sector_number += srb_entry->offset / 512;
  block_count -= srb_entry->offset / 512;

  NT_ASSERT(block_count > 0);

  /* look for pending writes that overlap this one */
  /* we get warnings from drbd if we don't */
  if (srb_entry->offset == 0) {
    for (i = 0; i < MAX_SHADOW_ENTRIES; i++) {
      PSCSI_REQUEST_BLOCK srb2;
      ULONGLONG sector_number2;
      ULONG block_count2;
      
      srb2 = xvdd->shadows[i].srb;
      if (!srb2)
        continue;
      if (decode_cdb_is_read(srb2))
        continue;
      block_count2 = decode_cdb_length(srb2);;
      block_count2 *= xvdd->bytes_per_sector / 512;
      sector_number2 = decode_cdb_sector(srb2);
      sector_number2 *= xvdd->bytes_per_sector / 512;
      
      if (sector_number < sector_number2 && sector_number + block_count <= sector_number2)
        continue;
      if (sector_number2 < sector_number && sector_number2 + block_count2 <= sector_number)
        continue;

      KdPrint((__DRIVER_NAME "     Concurrent outstanding write detected (%I64d, %d) (%I64d, %d)\n",
        sector_number, block_count, sector_number2, block_count2));
      break;
    }
    if (i != MAX_SHADOW_ENTRIES) {
      /* put the srb back at the start of the queue */
      InsertHeadList(&xvdd->srb_list, (PLIST_ENTRY)srb->SrbExtension);
      //if (dump_mode) FUNCTION_EXIT();
      return FALSE;
    }
  }
  
  shadow = get_shadow_from_freelist(xvdd);
  if (!shadow) {
    /* put the srb back at the start of the queue */
    InsertHeadList(&xvdd->srb_list, (PLIST_ENTRY)srb->SrbExtension);
    //if (dump_mode) FUNCTION_EXIT();
    return FALSE;
  }
  NT_ASSERT(!shadow->aligned_buffer_in_use);
  NT_ASSERT(!shadow->srb);
  shadow->req.sector_number = sector_number;
  shadow->req.handle = 0;
  shadow->req.operation = decode_cdb_is_read(srb)?BLKIF_OP_READ:BLKIF_OP_WRITE;
  shadow->req.nr_segments = 0;
  shadow->srb = srb;
  shadow->length = 0;
  shadow->system_address = system_address;
  shadow->reset = FALSE;

  if (!dump_mode) {
    if ((ULONG_PTR)shadow->system_address & 511) {
      xvdd->aligned_buffer_in_use = TRUE;
      /* limit to aligned_buffer_size */
      block_count = min(block_count, xvdd->aligned_buffer_size / 512);
      ptr = (PUCHAR)xvdd->aligned_buffer;
      if (!decode_cdb_is_read(srb))
        memcpy(ptr, shadow->system_address, block_count * 512);
      shadow->aligned_buffer_in_use = TRUE;
    } else {
      ptr = (PUCHAR)shadow->system_address;
      shadow->aligned_buffer_in_use = FALSE;
    }
  } else {
    NT_ASSERT(!((ULONG_PTR)shadow->system_address & 511));
    ptr = shadow->system_address;
    shadow->aligned_buffer_in_use = FALSE;
  }

  //KdPrint((__DRIVER_NAME "     sector_number = %d, block_count = %d\n", (ULONG)sector_number, block_count));
  //KdPrint((__DRIVER_NAME "     DataBuffer   = %p\n", srb->DataBuffer));
  //KdPrint((__DRIVER_NAME "     system_address   = %p\n", shadow->system_address));
  //KdPrint((__DRIVER_NAME "     offset   = %d, length = %d\n", srb_entry->offset, srb_entry->length));
  //KdPrint((__DRIVER_NAME "     ptr   = %p\n", ptr));

  //KdPrint((__DRIVER_NAME "     handle = %d\n", shadow->req.handle));
  //KdPrint((__DRIVER_NAME "     operation = %d\n", shadow->req.operation));
  
  remaining = block_count * 512;
  while (remaining > 0 && shadow->req.nr_segments < BLKIF_MAX_SEGMENTS_PER_REQUEST) {
    PHYSICAL_ADDRESS physical_address;

    if (!dump_mode) {
      physical_address = MmGetPhysicalAddress(ptr);
    } else {
#if 0
      ULONG length;       
      physical_address = StorPortGetPhysicalAddress(xvdd, srb, ptr, &length);
#endif
      physical_address = MmGetPhysicalAddress(ptr);
    }
    gref = XnGrantAccess(xvdd->handle,
           (ULONG)(physical_address.QuadPart >> PAGE_SHIFT), FALSE, INVALID_GRANT_REF, xvdd->grant_tag);
    if (gref == INVALID_GRANT_REF) {
      ULONG i;
      for (i = 0; i < shadow->req.nr_segments; i++) {
        XnEndAccess(xvdd->handle,
          shadow->req.seg[i].gref, FALSE, xvdd->grant_tag);
      }
      if (shadow->aligned_buffer_in_use) {
        shadow->aligned_buffer_in_use = FALSE;
        xvdd->aligned_buffer_in_use = FALSE;
      }
      /* put the srb back at the start of the queue */
      InsertHeadList(&xvdd->srb_list, (PLIST_ENTRY)srb_entry);
      put_shadow_on_freelist(xvdd, shadow);
      KdPrint((__DRIVER_NAME "     Out of gref's. Deferring\n"));
      /* TODO: what if there are no requests currently in progress to kick the queue again?? timer? */
      return FALSE;
    }
    offset = physical_address.LowPart & (PAGE_SIZE - 1);
    length = min(PAGE_SIZE - offset, remaining);
    NT_ASSERT((offset & 511) == 0);
    NT_ASSERT((length & 511) == 0);
    NT_ASSERT(offset + length <= PAGE_SIZE);
    shadow->req.seg[shadow->req.nr_segments].gref = gref;
    shadow->req.seg[shadow->req.nr_segments].first_sect = (UCHAR)(offset / 512);
    shadow->req.seg[shadow->req.nr_segments].last_sect = (UCHAR)(((offset + length) / 512) - 1);
    remaining -= length;
    ptr += length;
    shadow->length += length;
    shadow->req.nr_segments++;
  }
  srb_entry->offset += shadow->length;
  srb_entry->outstanding_requests++;
  if (srb_entry->offset < srb_entry->length) {
    if (dump_mode) KdPrint((__DRIVER_NAME "     inserting back into list\n"));
    /* put the srb back at the start of the queue to continue on the next request */
    InsertHeadList(&xvdd->srb_list, (PLIST_ENTRY)srb_entry);
  }
  XenVbd_PutRequest(xvdd, &shadow->req);
  //if (dump_mode)
  //FUNCTION_EXIT();
  return TRUE;
}


static ULONG
XenVbd_FillModePage(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  PMODE_PARAMETER_HEADER parameter_header = NULL;
  PMODE_PARAMETER_HEADER10 parameter_header10 = NULL;
  PMODE_PARAMETER_BLOCK param_block;
  PMODE_FORMAT_PAGE format_page;
  ULONG offset = 0;
  UCHAR buffer[1024];
  BOOLEAN valid_page = FALSE;
  BOOLEAN cdb_llbaa;
  BOOLEAN cdb_dbd;
  UCHAR cdb_page_code;
  USHORT cdb_allocation_length;

  UNREFERENCED_PARAMETER(xvdd);

  RtlZeroMemory(srb->DataBuffer, srb->DataTransferLength);
  RtlZeroMemory(buffer, ARRAY_SIZE(buffer));
  offset = 0;

  //KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));
  
  switch (srb->Cdb[0])
  {
  case SCSIOP_MODE_SENSE:
    cdb_llbaa = FALSE;
    cdb_dbd = (BOOLEAN)!!(srb->Cdb[1] & 8);
    cdb_page_code = srb->Cdb[2] & 0x3f;
    cdb_allocation_length = srb->Cdb[4];
    parameter_header = (PMODE_PARAMETER_HEADER)&buffer[offset];
    parameter_header->MediumType = 0;
    parameter_header->DeviceSpecificParameter = 0;
    if (xvdd->device_mode == XENVBD_DEVICEMODE_READ)
    {
      KdPrint((__DRIVER_NAME " Mode sense to a read only disk.\n"));
      parameter_header->DeviceSpecificParameter |= MODE_DSP_WRITE_PROTECT; 
    }
    offset += sizeof(MODE_PARAMETER_HEADER);
    break;
  case SCSIOP_MODE_SENSE10:
    cdb_llbaa = (BOOLEAN)!!(srb->Cdb[1] & 16);
    cdb_dbd = (BOOLEAN)!!(srb->Cdb[1] & 8);
    cdb_page_code = srb->Cdb[2] & 0x3f;
    cdb_allocation_length = (srb->Cdb[7] << 8) | srb->Cdb[8];
    parameter_header10 = (PMODE_PARAMETER_HEADER10)&buffer[offset];
    parameter_header10->MediumType = 0;
    parameter_header10->DeviceSpecificParameter = 0;
    if (xvdd->device_mode == XENVBD_DEVICEMODE_READ)
    {
      KdPrint((__DRIVER_NAME " Mode sense to a read only disk.\n"));
      parameter_header10->DeviceSpecificParameter |= MODE_DSP_WRITE_PROTECT; 
    }
    offset += sizeof(MODE_PARAMETER_HEADER10);
    break;
  default:
    KdPrint((__DRIVER_NAME "     SCSIOP_MODE_SENSE_WTF (%02x)\n", (ULONG)srb->Cdb[0]));
    return FALSE;
  }  
  
  if (!cdb_dbd)
  {
    param_block = (PMODE_PARAMETER_BLOCK)&buffer[offset];
    if (xvdd->device_type == XENVBD_DEVICETYPE_DISK)
    {
      if (xvdd->total_sectors >> 32) 
      {
        param_block->DensityCode = 0xff;
        param_block->NumberOfBlocks[0] = 0xff;
        param_block->NumberOfBlocks[1] = 0xff;
        param_block->NumberOfBlocks[2] = 0xff;
      }
      else
      {
        param_block->DensityCode = (UCHAR)((xvdd->total_sectors >> 24) & 0xff);
        param_block->NumberOfBlocks[0] = (UCHAR)((xvdd->total_sectors >> 16) & 0xff);
        param_block->NumberOfBlocks[1] = (UCHAR)((xvdd->total_sectors >> 8) & 0xff);
        param_block->NumberOfBlocks[2] = (UCHAR)((xvdd->total_sectors >> 0) & 0xff);
      }
      param_block->BlockLength[0] = (UCHAR)((xvdd->bytes_per_sector >> 16) & 0xff);
      param_block->BlockLength[1] = (UCHAR)((xvdd->bytes_per_sector >> 8) & 0xff);
      param_block->BlockLength[2] = (UCHAR)((xvdd->bytes_per_sector >> 0) & 0xff);
    }
    offset += sizeof(MODE_PARAMETER_BLOCK);
  }
  switch (srb->Cdb[0])
  {
  case SCSIOP_MODE_SENSE:
    parameter_header->BlockDescriptorLength = (UCHAR)(offset - sizeof(MODE_PARAMETER_HEADER));
    break;
  case SCSIOP_MODE_SENSE10:
    parameter_header10->BlockDescriptorLength[0] = (UCHAR)((offset - sizeof(MODE_PARAMETER_HEADER10)) >> 8);
    parameter_header10->BlockDescriptorLength[1] = (UCHAR)(offset - sizeof(MODE_PARAMETER_HEADER10));
    break;
  }
  if (xvdd->device_type == XENVBD_DEVICETYPE_DISK && (cdb_page_code == MODE_PAGE_FORMAT_DEVICE || cdb_page_code == MODE_SENSE_RETURN_ALL))
  {
    valid_page = TRUE;
    format_page = (PMODE_FORMAT_PAGE)&buffer[offset];
    format_page->PageCode = MODE_PAGE_FORMAT_DEVICE;
    format_page->PageLength = sizeof(MODE_FORMAT_PAGE) - FIELD_OFFSET(MODE_FORMAT_PAGE, PageLength);
    /* 256 sectors per track */
    format_page->SectorsPerTrack[0] = 0x01;
    format_page->SectorsPerTrack[1] = 0x00;
    /* xxx bytes per sector */
    format_page->BytesPerPhysicalSector[0] = (UCHAR)(xvdd->bytes_per_sector >> 8);
    format_page->BytesPerPhysicalSector[1] = (UCHAR)(xvdd->bytes_per_sector & 0xff);
    format_page->HardSectorFormating = TRUE;
    format_page->SoftSectorFormating = TRUE;
    offset += sizeof(MODE_FORMAT_PAGE);
  }
  if (xvdd->device_type == XENVBD_DEVICETYPE_DISK && (cdb_page_code == MODE_PAGE_CACHING || cdb_page_code == MODE_SENSE_RETURN_ALL))
  {
    PMODE_CACHING_PAGE caching_page;
    valid_page = TRUE;
    caching_page = (PMODE_CACHING_PAGE)&buffer[offset];
    caching_page->PageCode = MODE_PAGE_CACHING;
    caching_page->PageLength = sizeof(MODE_CACHING_PAGE) - FIELD_OFFSET(MODE_CACHING_PAGE, PageLength);
    // caching_page-> // all zeros is just fine... maybe
    offset += sizeof(MODE_CACHING_PAGE);
  }
  if (xvdd->device_type == XENVBD_DEVICETYPE_DISK && (cdb_page_code == MODE_PAGE_MEDIUM_TYPES || cdb_page_code == MODE_SENSE_RETURN_ALL))
  {
    PUCHAR medium_types_page;
    valid_page = TRUE;
    medium_types_page = &buffer[offset];
    medium_types_page[0] = MODE_PAGE_MEDIUM_TYPES;
    medium_types_page[1] = 0x06;
    medium_types_page[2] = 0;
    medium_types_page[3] = 0;
    medium_types_page[4] = 0;
    medium_types_page[5] = 0;
    medium_types_page[6] = 0;
    medium_types_page[7] = 0;
    offset += 8;
  }
  switch (srb->Cdb[0])
  {
  case SCSIOP_MODE_SENSE:
    parameter_header->ModeDataLength = (UCHAR)(offset - 1);
    break;
  case SCSIOP_MODE_SENSE10:
    parameter_header10->ModeDataLength[0] = (UCHAR)((offset - 2) >> 8);
    parameter_header10->ModeDataLength[1] = (UCHAR)(offset - 2);
    break;
  }

  if (!valid_page && cdb_page_code != MODE_SENSE_RETURN_ALL)
  {
    srb->SrbStatus = SRB_STATUS_ERROR;
  }
  else if(offset < srb->DataTransferLength)
    srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
  else
    srb->SrbStatus = SRB_STATUS_SUCCESS;
  srb->DataTransferLength = min(srb->DataTransferLength, offset);
  srb->ScsiStatus = 0;
  memcpy(srb->DataBuffer, buffer, srb->DataTransferLength);
  
  //FUNCTION_EXIT();

  return TRUE;
}

static BOOLEAN
XenVbd_ResetBus(PXENVBD_DEVICE_DATA xvdd, ULONG PathId) {
  //srb_list_entry_t *srb_entry;
  int i;
  /* need to make sure that each SRB is only reset once */
  LIST_ENTRY srb_reset_list;
  PLIST_ENTRY list_entry;
  //STOR_LOCK_HANDLE lock_handle;

  UNREFERENCED_PARAMETER(PathId);

  FUNCTION_ENTER();
  
  if (dump_mode) {
    FUNCTION_MSG("dump mode - doing nothing\n");
    FUNCTION_EXIT();
    return TRUE;
  }

  /* It appears that the StartIo spinlock is already held at this point */

  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  xvdd->aligned_buffer_in_use = FALSE;
  
  InitializeListHead(&srb_reset_list);
  
  while((list_entry = RemoveHeadList(&xvdd->srb_list)) != &xvdd->srb_list) {
    #if DBG
    srb_list_entry_t *srb_entry = CONTAINING_RECORD(list_entry, srb_list_entry_t, list_entry);
    KdPrint((__DRIVER_NAME "     adding queued SRB %p to reset list\n", srb_entry->srb));
    #endif
    InsertTailList(&srb_reset_list, list_entry);
  }
  
  for (i = 0; i < MAX_SHADOW_ENTRIES; i++) {
    if (xvdd->shadows[i].srb) {
      srb_list_entry_t *srb_entry = xvdd->shadows[i].srb->SrbExtension;
      for (list_entry = srb_reset_list.Flink; list_entry != &srb_reset_list; list_entry = list_entry->Flink) {
        if (list_entry == &srb_entry->list_entry)
          break;
      }
      if (list_entry == &srb_reset_list) {
        KdPrint((__DRIVER_NAME "     adding in-flight SRB %p to reset list\n", srb_entry->srb));
        InsertTailList(&srb_reset_list, &srb_entry->list_entry);
      }
      /* set reset here so that the interrupt won't do anything with the srb but will dispose of the shadow entry correctly */
      xvdd->shadows[i].reset = TRUE;
      xvdd->shadows[i].srb = NULL;
      xvdd->shadows[i].aligned_buffer_in_use = FALSE;
    }
  }

  while((list_entry = RemoveHeadList(&srb_reset_list)) != &srb_reset_list) {
    srb_list_entry_t *srb_entry = CONTAINING_RECORD(list_entry, srb_list_entry_t, list_entry);
    srb_entry->srb->SrbStatus = SRB_STATUS_BUS_RESET;
    KdPrint((__DRIVER_NAME "     completing SRB %p with status SRB_STATUS_BUS_RESET\n", srb_entry->srb));
    SxxxPortNotification(RequestComplete, xvdd, srb_entry->srb);
  }

  /* send a notify to Dom0 just in case it was missed for some reason (which should _never_ happen normally but could in dump mode) */
  XnNotify(xvdd->handle, xvdd->event_channel);

  SxxxPortNotification(NextLuRequest, xvdd, 0, 0, 0);
  FUNCTION_EXIT();

  return TRUE;
}

/* called with StartIo lock held */
VOID
XenVbd_ProcessSrbList(PXENVBD_DEVICE_DATA xvdd) {
  PUCHAR data_buffer;
  #ifdef _NTSTORPORT_
  PSCSI_PNP_REQUEST_BLOCK sprb;
  PSCSI_POWER_REQUEST_BLOCK spwrb;
  PMINIPORT_DUMP_POINTERS dump_pointers;
  #endif
  PCDB cdb;
  ULONG data_transfer_length;
  UCHAR srb_status = SRB_STATUS_PENDING;
  BOOLEAN notify = FALSE;
  PSCSI_REQUEST_BLOCK srb;
  srb_list_entry_t *srb_entry;
  PSRB_IO_CONTROL sic;

  if (xvdd->device_state != DEVICE_STATE_ACTIVE) {
    FUNCTION_MSG("Not yet active - state = %d\n", xvdd->device_state);
    return;
  }

  while(!xvdd->aligned_buffer_in_use && xvdd->shadow_free && (srb_entry = (srb_list_entry_t *)RemoveHeadList(&xvdd->srb_list)) != (srb_list_entry_t *)&xvdd->srb_list) {
    srb = srb_entry->srb;
    if (xvdd->device_state == DEVICE_STATE_INACTIVE) {
      /* need to check again as may have been initialising when this srb was put on the list */
      FUNCTION_MSG("Inactive Device (in ProcessSrbList)\n");
      srb->SrbStatus = SRB_STATUS_NO_DEVICE;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      continue;
    }

    data_transfer_length = srb->DataTransferLength;
    
    switch (srb->Function)
    {
    case SRB_FUNCTION_EXECUTE_SCSI:
      cdb = (PCDB)srb->Cdb;

      switch(cdb->CDB6GENERIC.OperationCode) {
      case SCSIOP_TEST_UNIT_READY:
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = TEST_UNIT_READY\n"));
        srb_status = SRB_STATUS_SUCCESS;
        srb->ScsiStatus = 0;
        break;
      case SCSIOP_INQUIRY:
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = INQUIRY\n"));
  //      KdPrint((__DRIVER_NAME "     (LUN = %d, EVPD = %d, Page Code = %02X)\n", srb->Cdb[1] >> 5, srb->Cdb[1] & 1, srb->Cdb[2]));
  //      KdPrint((__DRIVER_NAME "     (Length = %d)\n", srb->DataTransferLength));
        
        data_buffer = srb->DataBuffer;
        RtlZeroMemory(data_buffer, srb->DataTransferLength);
        srb_status = SRB_STATUS_SUCCESS;
        switch (xvdd->device_type)
        {
        case XENVBD_DEVICETYPE_DISK:
          if ((srb->Cdb[1] & 1) == 0) {
            if (srb->Cdb[2]) {
              srb_status = SRB_STATUS_ERROR;
            } else {
              PINQUIRYDATA id = (PINQUIRYDATA)data_buffer;
              id->DeviceType = DIRECT_ACCESS_DEVICE;
              id->Versions = 5; /* SPC-3 */
              id->ResponseDataFormat = 2; /* not sure about this but WHQL complains otherwise */
              id->HiSupport = 1; /* WHQL test says we should set this */
              //id->AdditionalLength = FIELD_OFFSET(INQUIRYDATA, VendorSpecific) - FIELD_OFFSET(INQUIRYDATA, AdditionalLength);
              id->AdditionalLength = sizeof(INQUIRYDATA) - FIELD_OFFSET(INQUIRYDATA, AdditionalLength) - 1;
              id->CommandQueue = 1;
              memcpy(id->VendorId, SCSI_DEVICE_MANUFACTURER, 8); // vendor id
              memcpy(id->ProductId, SCSI_DISK_MODEL, 16); // product id
              memcpy(id->ProductRevisionLevel, "0000", 4); // product revision level
              data_transfer_length = FIELD_OFFSET(INQUIRYDATA, VendorSpecific);
            }
          } else {
            switch (srb->Cdb[2]) {
            case VPD_SUPPORTED_PAGES: /* list of pages we support */
              FUNCTION_MSG("VPD_SUPPORTED_PAGES\n");
              data_buffer[0] = DIRECT_ACCESS_DEVICE;
              data_buffer[1] = VPD_SUPPORTED_PAGES;
              data_buffer[2] = 0x00;
              data_buffer[3] = 4;
              data_buffer[4] = VPD_SUPPORTED_PAGES;
              data_buffer[5] = VPD_SERIAL_NUMBER;
              data_buffer[6] = VPD_DEVICE_IDENTIFIERS;
              data_buffer[7] = VPD_BLOCK_LIMITS;
              data_transfer_length = 8;
              break;
            case VPD_SERIAL_NUMBER: /* serial number */
              FUNCTION_MSG("VPD_SERIAL_NUMBER\n");
              data_buffer[0] = DIRECT_ACCESS_DEVICE;
              data_buffer[1] = VPD_SERIAL_NUMBER;
              data_buffer[2] = 0x00;
              data_buffer[3] = 8;
              memset(&data_buffer[4], ' ', 8);
              data_transfer_length = 12;
              break;
            case VPD_DEVICE_IDENTIFIERS: /* identification - we don't support any so just return zero */
              FUNCTION_MSG("VPD_DEVICE_IDENTIFIERS\n");
              data_buffer[0] = DIRECT_ACCESS_DEVICE;
              data_buffer[1] = VPD_DEVICE_IDENTIFIERS;
              data_buffer[2] = 0x00;
              //data_buffer[3] = 4 + (UCHAR)strlen(xvdd->vectors.path); /* length */
              data_buffer[4] = 2; /* ASCII */
              data_buffer[5] = 1; /* VendorId */
              data_buffer[6] = 0;
              //data_buffer[7] = (UCHAR)strlen(xvdd->vectors.path);
              //memcpy(&data_buffer[8], xvdd->vectors.path, strlen(xvdd->vectors.path));
              //data_transfer_length = (ULONG)(8 + strlen(xvdd->vectors.path));
              break;
            case VPD_BLOCK_LIMITS: /* to indicate support for UNMAP (TRIM/DISCARD) */
              FUNCTION_MSG("VPD_BLOCK_LIMITS\n");
              // max descriptors = 1
              // max sectors = 0xFFFFFFFF
              // granularity = from xenbus
              // alignment = from xenbus(?)
              srb_status = SRB_STATUS_ERROR;
              break;
            default:
              FUNCTION_MSG("Unknown Page %02x requested\n", srb->Cdb[2]);
              srb_status = SRB_STATUS_ERROR;
              break;
            }
          }
          break;
        case XENVBD_DEVICETYPE_CDROM:
          if ((srb->Cdb[1] & 1) == 0)
          {
            PINQUIRYDATA id = (PINQUIRYDATA)data_buffer;
            id->DeviceType = READ_ONLY_DIRECT_ACCESS_DEVICE;
            id->RemovableMedia = 1;
            id->Versions = 3;
            id->ResponseDataFormat = 0;
            id->AdditionalLength = FIELD_OFFSET(INQUIRYDATA, VendorSpecific) - FIELD_OFFSET(INQUIRYDATA, AdditionalLength);
            id->CommandQueue = 1;
            memcpy(id->VendorId, SCSI_DEVICE_MANUFACTURER, 8); // vendor id
            memcpy(id->ProductId, SCSI_CDROM_MODEL, 16); // product id
            memcpy(id->ProductRevisionLevel, "0000", 4); // product revision level
            data_transfer_length = sizeof(INQUIRYDATA);
          }
          else
          {
            switch (srb->Cdb[2])
            {
            case 0x00:
              data_buffer[0] = READ_ONLY_DIRECT_ACCESS_DEVICE;
              data_buffer[1] = 0x00;
              data_buffer[2] = 0x00;
              data_buffer[3] = 2;
              data_buffer[4] = 0x00;
              data_buffer[5] = 0x80;
              data_transfer_length = 6;
              break;
            case 0x80:
              data_buffer[0] = READ_ONLY_DIRECT_ACCESS_DEVICE;
              data_buffer[1] = 0x80;
              data_buffer[2] = 0x00;
              data_buffer[3] = 8;
              data_buffer[4] = 0x31;
              data_buffer[5] = 0x32;
              data_buffer[6] = 0x33;
              data_buffer[7] = 0x34;
              data_buffer[8] = 0x35;
              data_buffer[9] = 0x36;
              data_buffer[10] = 0x37;
              data_buffer[11] = 0x38;
              data_transfer_length = 12;
              break;
            default:
              //KdPrint((__DRIVER_NAME "     Unknown Page %02x requested\n", srb->Cdb[2]));
              srb_status = SRB_STATUS_ERROR;
              break;
            }
          }
          break;
        default:
          //KdPrint((__DRIVER_NAME "     Unknown DeviceType %02x requested\n", xvdd->device_type));
          srb_status = SRB_STATUS_ERROR;
          break;
        }
        break;
      case SCSIOP_READ_CAPACITY:
        //if (dump_mode)
        //  KdPrint((__DRIVER_NAME "     Command = READ_CAPACITY\n"));
        //KdPrint((__DRIVER_NAME "       LUN = %d, RelAdr = %d\n", srb->Cdb[1] >> 4, srb->Cdb[1] & 1));
        //KdPrint((__DRIVER_NAME "       LBA = %02x%02x%02x%02x\n", srb->Cdb[2], srb->Cdb[3], srb->Cdb[4], srb->Cdb[5]));
        //KdPrint((__DRIVER_NAME "       PMI = %d\n", srb->Cdb[8] & 1));
        //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, srb, srb->DataBuffer, &data_buffer_length).QuadPart);
        data_buffer = srb->DataBuffer;
        RtlZeroMemory(data_buffer, srb->DataTransferLength);
        if ((xvdd->total_sectors - 1) >> 32)
        {
          data_buffer[0] = 0xff;
          data_buffer[1] = 0xff;
          data_buffer[2] = 0xff;
          data_buffer[3] = 0xff;
        }
        else
        {
          data_buffer[0] = (unsigned char)((xvdd->total_sectors - 1) >> 24) & 0xff;
          data_buffer[1] = (unsigned char)((xvdd->total_sectors - 1) >> 16) & 0xff;
          data_buffer[2] = (unsigned char)((xvdd->total_sectors - 1) >> 8) & 0xff;
          data_buffer[3] = (unsigned char)((xvdd->total_sectors - 1) >> 0) & 0xff;
        }
        data_buffer[4] = (unsigned char)(xvdd->bytes_per_sector >> 24) & 0xff;
        data_buffer[5] = (unsigned char)(xvdd->bytes_per_sector >> 16) & 0xff;
        data_buffer[6] = (unsigned char)(xvdd->bytes_per_sector >> 8) & 0xff;
        data_buffer[7] = (unsigned char)(xvdd->bytes_per_sector >> 0) & 0xff;
        data_transfer_length = 8;
        srb->ScsiStatus = 0;
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_READ_CAPACITY16:
        //if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = READ_CAPACITY16\n"));
        //KdPrint((__DRIVER_NAME "       LUN = %d, RelAdr = %d\n", srb->Cdb[1] >> 4, srb->Cdb[1] & 1));
        //KdPrint((__DRIVER_NAME "       LBA = %02x%02x%02x%02x\n", srb->Cdb[2], srb->Cdb[3], srb->Cdb[4], srb->Cdb[5]));
        //KdPrint((__DRIVER_NAME "       PMI = %d\n", srb->Cdb[8] & 1));
        data_buffer = srb->DataBuffer;
        RtlZeroMemory(data_buffer, srb->DataTransferLength);
        data_buffer[0] = (unsigned char)((xvdd->total_sectors - 1) >> 56) & 0xff;
        data_buffer[1] = (unsigned char)((xvdd->total_sectors - 1) >> 48) & 0xff;
        data_buffer[2] = (unsigned char)((xvdd->total_sectors - 1) >> 40) & 0xff;
        data_buffer[3] = (unsigned char)((xvdd->total_sectors - 1) >> 32) & 0xff;
        data_buffer[4] = (unsigned char)((xvdd->total_sectors - 1) >> 24) & 0xff;
        data_buffer[5] = (unsigned char)((xvdd->total_sectors - 1) >> 16) & 0xff;
        data_buffer[6] = (unsigned char)((xvdd->total_sectors - 1) >> 8) & 0xff;
        data_buffer[7] = (unsigned char)((xvdd->total_sectors - 1) >> 0) & 0xff;
        data_buffer[8] = (unsigned char)(xvdd->bytes_per_sector >> 24) & 0xff;
        data_buffer[9] = (unsigned char)(xvdd->bytes_per_sector >> 16) & 0xff;
        data_buffer[10] = (unsigned char)(xvdd->bytes_per_sector >> 8) & 0xff;
        data_buffer[11] = (unsigned char)(xvdd->bytes_per_sector >> 0) & 0xff;
        data_buffer[12] = 0;
        switch (xvdd->hw_bytes_per_sector / xvdd->bytes_per_sector) {
        case 1:
          data_buffer[13] = 0; /* 512 byte hardware sectors */
          break;
        case 2:
          data_buffer[13] = 1; /* 1024 byte hardware sectors */
          break;
        case 3:
          data_buffer[13] = 2; /* 2048 byte hardware sectors */
          break;
        case 4:
          data_buffer[13] = 3; /* 4096 byte hardware sectors */
          break;
        default:
          data_buffer[13] = 0; /* 512 byte hardware sectors */
          KdPrint((__DRIVER_NAME "     Unknown logical blocks per physical block %d (%d / %d)\n", xvdd->hw_bytes_per_sector / xvdd->bytes_per_sector, xvdd->hw_bytes_per_sector, xvdd->bytes_per_sector));
          break;
        }
        data_buffer[14] = 0xC0; //0;
        data_buffer[15] = 0;
        data_transfer_length = 16;
        srb->ScsiStatus = 0;
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_MODE_SENSE:
      case SCSIOP_MODE_SENSE10:
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = MODE_SENSE (DBD = %d, PC = %d, Page Code = %02x)\n", srb->Cdb[1] & 0x08, srb->Cdb[2] & 0xC0, srb->Cdb[2] & 0x3F));
        data_transfer_length = XenVbd_FillModePage(xvdd, srb);
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_READ:
      case SCSIOP_READ16:
      case SCSIOP_WRITE:
      case SCSIOP_WRITE16:
        //FUNCTION_MSG("srb = %p\n", srb);
        if (XenVbd_PutSrbOnRing(xvdd, srb)) {
          notify = TRUE;
        }
        break;
      case SCSIOP_WRITE_SAME:
      case SCSIOP_WRITE_SAME16:
        /* not yet supported */
        FUNCTION_MSG("WRITE_SAME\n");
        break;
      case SCSIOP_UNMAP:
        /* not yet supported */
        FUNCTION_MSG("UNMAP\n");
        break;
      case SCSIOP_VERIFY:
      case SCSIOP_VERIFY16:
        // Should we do more here?
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = VERIFY\n"));
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_REPORT_LUNS:
        //if (dump_mode)
          FUNCTION_MSG("Command = REPORT_LUNS\n");
        switch (srb->Cdb[2]) {
        case 1:
          FUNCTION_MSG(" SELECT REPORT = %d\n", srb->Cdb[2] & 255);
          break;
        default:
          FUNCTION_MSG(" SELECT REPORT = %d\n", srb->Cdb[2] & 255);
          break;
        }
        FUNCTION_MSG(" ALLOCATION LENGTH = %d\n", (srb->Cdb[6] << 24)|(srb->Cdb[7] << 16)|(srb->Cdb[8] << 8)|(srb->Cdb[9]));
        data_buffer = srb->DataBuffer;
        RtlZeroMemory(data_buffer, srb->DataTransferLength);
        data_buffer[3] = 8; /* 1 lun */
        /* rest of the data is blank */
        data_transfer_length = 16;
        srb->ScsiStatus = 0;
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_REQUEST_SENSE:
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = REQUEST_SENSE\n"));
        data_transfer_length = XenVbd_MakeSense(xvdd, srb, xvdd->last_sense_key, xvdd->last_additional_sense_code);
        srb_status = SRB_STATUS_SUCCESS;
        break;      
      case SCSIOP_READ_TOC:
        if (dump_mode)
          KdPrint((__DRIVER_NAME "     Command = READ_TOC\n"));
        data_buffer = srb->DataBuffer;
  /*
  #define READ_TOC_FORMAT_TOC         0x00
  #define READ_TOC_FORMAT_SESSION     0x01
  #define READ_TOC_FORMAT_FULL_TOC    0x02
  #define READ_TOC_FORMAT_PMA         0x03
  #define READ_TOC_FORMAT_ATIP        0x04
  */
  //      KdPrint((__DRIVER_NAME "     Msf = %d\n", cdb->READ_TOC.Msf));
  //      KdPrint((__DRIVER_NAME "     LogicalUnitNumber = %d\n", cdb->READ_TOC.LogicalUnitNumber));
  //      KdPrint((__DRIVER_NAME "     Format2 = %d\n", cdb->READ_TOC.Format2));
  //      KdPrint((__DRIVER_NAME "     StartingTrack = %d\n", cdb->READ_TOC.StartingTrack));
  //      KdPrint((__DRIVER_NAME "     AllocationLength = %d\n", (cdb->READ_TOC.AllocationLength[0] << 8) | cdb->READ_TOC.AllocationLength[1]));
  //      KdPrint((__DRIVER_NAME "     Control = %d\n", cdb->READ_TOC.Control));
  //      KdPrint((__DRIVER_NAME "     Format = %d\n", cdb->READ_TOC.Format));
        switch (cdb->READ_TOC.Format2)
        {
        case READ_TOC_FORMAT_TOC:
          data_buffer[0] = 0; // length MSB
          data_buffer[1] = 10; // length LSB
          data_buffer[2] = 1; // First Track
          data_buffer[3] = 1; // Last Track
          data_buffer[4] = 0; // Reserved
          data_buffer[5] = 0x14; // current position data + uninterrupted data
          data_buffer[6] = 1; // last complete track
          data_buffer[7] = 0; // reserved
          data_buffer[8] = 0; // MSB Block
          data_buffer[9] = 0;
          data_buffer[10] = 0;
          data_buffer[11] = 0; // LSB Block
          data_transfer_length = 12;
          srb_status = SRB_STATUS_SUCCESS;
          break;
        case READ_TOC_FORMAT_SESSION:
        case READ_TOC_FORMAT_FULL_TOC:
        case READ_TOC_FORMAT_PMA:
        case READ_TOC_FORMAT_ATIP:
          srb_status = SRB_STATUS_ERROR;
          break;
        default:
          srb_status = SRB_STATUS_ERROR;
          break;
        }
        break;
      case SCSIOP_START_STOP_UNIT:
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_START_STOP_UNIT\n"));
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_RESERVE_UNIT:
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_RESERVE_UNIT\n"));
        srb_status = SRB_STATUS_SUCCESS;
        break;
      case SCSIOP_RELEASE_UNIT:
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_RELEASE_UNIT\n"));
        srb_status = SRB_STATUS_SUCCESS;
        break;
      default:
        KdPrint((__DRIVER_NAME "     Unhandled EXECUTE_SCSI Command = %02X\n", srb->Cdb[0]));
        xvdd->last_sense_key = SCSI_SENSE_ILLEGAL_REQUEST;
        srb_status = SRB_STATUS_ERROR;
        break;
      }
      if (srb_status == SRB_STATUS_ERROR) {
        KdPrint((__DRIVER_NAME "     EXECUTE_SCSI Command = %02X returned error %02x\n", srb->Cdb[0], xvdd->last_sense_key));
        if (xvdd->last_sense_key == SCSI_SENSE_NO_SENSE) {
          xvdd->last_sense_key = SCSI_SENSE_ILLEGAL_REQUEST;
          xvdd->last_additional_sense_code = SCSI_ADSENSE_INVALID_CDB;
        }
        srb->SrbStatus = srb_status;
        srb->ScsiStatus = 0x02;
        XenVbd_MakeAutoSense(xvdd, srb);
        SxxxPortNotification(RequestComplete, xvdd, srb);
      } else if (srb_status != SRB_STATUS_PENDING) {
        if (data_transfer_length > srb->DataTransferLength)
          KdPrint((__DRIVER_NAME "     data_transfer_length too big - %d > %d\n", data_transfer_length, srb->DataTransferLength));
        
        if (srb_status == SRB_STATUS_SUCCESS && data_transfer_length < srb->DataTransferLength) {
          srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
          srb->DataTransferLength = data_transfer_length;
        } else {
          srb->SrbStatus = srb_status;
        }
        xvdd->last_sense_key = SCSI_SENSE_NO_SENSE;
        xvdd->last_additional_sense_code = SCSI_ADSENSE_NO_SENSE;
        SxxxPortNotification(RequestComplete, xvdd, srb);
      }
      break;
    case SRB_FUNCTION_FLUSH:
      KdPrint((__DRIVER_NAME "     SRB_FUNCTION_FLUSH %p, xvdd->shadow_free = %d\n", srb, xvdd->shadow_free));
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    #ifdef _NTSTORPORT_      
    case SRB_FUNCTION_PNP:
      KdPrint((__DRIVER_NAME "     SRB_FUNCTION_PNP\n"));
      sprb = (PSCSI_PNP_REQUEST_BLOCK)srb;
      switch (sprb->PnPAction)
      {
      case StorStartDevice:
        KdPrint((__DRIVER_NAME "      StorStartDevice\n"));
        break;
      case StorRemoveDevice:
        KdPrint((__DRIVER_NAME "      StorRemoveDevice\n"));
        break;
      case StorStopDevice:
        KdPrint((__DRIVER_NAME "      StorStopDevice\n"));
        break;
      case StorQueryCapabilities:
        KdPrint((__DRIVER_NAME "      StorQueryCapabilities\n"));
        break;
      case StorFilterResourceRequirements:
        KdPrint((__DRIVER_NAME "      StorFilterResourceRequirements\n"));
        break;
      default:
        KdPrint((__DRIVER_NAME "      Stor%d\n", sprb->PnPAction));
        break;
      }
      KdPrint((__DRIVER_NAME "      SrbPnPFlags = %08x\n", sprb->SrbPnPFlags));
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
      
    case SRB_FUNCTION_POWER:
      KdPrint((__DRIVER_NAME "     SRB_FUNCTION_POWER\n"));
      spwrb = (PSCSI_POWER_REQUEST_BLOCK)srb;
      switch (spwrb->PowerAction)
      {
      case StorPowerActionNone:
        KdPrint((__DRIVER_NAME "      StorPowerActionNone\n"));
        break;
      case StorPowerActionReserved:
        KdPrint((__DRIVER_NAME "      StorPowerActionReserved\n"));
        break;
      case StorPowerActionSleep:
        KdPrint((__DRIVER_NAME "      StorPowerActionSleep\n"));
        break;
      case StorPowerActionHibernate:
        KdPrint((__DRIVER_NAME "      StorPowerActionHibernate\n"));
        break;
      case StorPowerActionShutdown:
        KdPrint((__DRIVER_NAME "      StorPowerActionShutdown\n"));
        break;
      case StorPowerActionShutdownReset:
        KdPrint((__DRIVER_NAME "      StorPowerActionShutdownReset\n"));
        break;
      case StorPowerActionShutdownOff:
        KdPrint((__DRIVER_NAME "      StorPowerActionShutdownOff\n"));
        break;
      case StorPowerActionWarmEject:
        KdPrint((__DRIVER_NAME "      StorPowerActionWarmEject\n"));
        break;
      default:
        KdPrint((__DRIVER_NAME "      Stor%d\n", spwrb->PowerAction));
        break;
      }
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    case SRB_FUNCTION_DUMP_POINTERS:
      KdPrint((__DRIVER_NAME "     SRB_FUNCTION_DUMP_POINTERS\n"));
      KdPrint((__DRIVER_NAME "     DataTransferLength = %d\n", srb->DataTransferLength));
      dump_pointers = srb->DataBuffer;
      KdPrint((__DRIVER_NAME "      Version = %d\n", dump_pointers->Version));
      KdPrint((__DRIVER_NAME "      Size = %d\n", dump_pointers->Size));
      KdPrint((__DRIVER_NAME "      DriverName = %S\n", dump_pointers->DriverName));
      KdPrint((__DRIVER_NAME "      AdapterObject = %p\n", dump_pointers->AdapterObject));
      KdPrint((__DRIVER_NAME "      MappedRegisterBase = %d\n", dump_pointers->MappedRegisterBase));
      KdPrint((__DRIVER_NAME "      CommonBufferSize = %d\n", dump_pointers->CommonBufferSize));
      KdPrint((__DRIVER_NAME "      MiniportPrivateDumpData = %p\n", dump_pointers->MiniportPrivateDumpData));
      KdPrint((__DRIVER_NAME "      SystemIoBusNumber = %d\n", dump_pointers->SystemIoBusNumber));
      KdPrint((__DRIVER_NAME "      AdapterInterfaceType = %d\n", dump_pointers->AdapterInterfaceType));
      KdPrint((__DRIVER_NAME "      MaximumTransferLength = %d\n", dump_pointers->MaximumTransferLength));
      KdPrint((__DRIVER_NAME "      NumberOfPhysicalBreaks = %d\n", dump_pointers->NumberOfPhysicalBreaks));
      KdPrint((__DRIVER_NAME "      AlignmentMask = %d\n", dump_pointers->AlignmentMask));
      KdPrint((__DRIVER_NAME "      NumberOfAccessRanges = %d\n", dump_pointers->NumberOfAccessRanges));
      KdPrint((__DRIVER_NAME "      NumberOfBuses = %d\n", dump_pointers->NumberOfBuses));
      KdPrint((__DRIVER_NAME "      Master = %d\n", dump_pointers->Master));
      KdPrint((__DRIVER_NAME "      MapBuffers = %d\n", dump_pointers->MapBuffers));
      KdPrint((__DRIVER_NAME "      MaximumNumberOfTargets = %d\n", dump_pointers->MaximumNumberOfTargets));

      dump_pointers->Version = DUMP_MINIPORT_VERSION_1;
      dump_pointers->Size = sizeof(MINIPORT_DUMP_POINTERS);
      RtlStringCchCopyW(dump_pointers->DriverName, DUMP_MINIPORT_NAME_LENGTH, L"xenvbd.sys");
      dump_pointers->AdapterObject = NULL;
      dump_pointers->MappedRegisterBase = 0;
      dump_pointers->CommonBufferSize = 0;
      dump_pointers->MiniportPrivateDumpData = xvdd;
      dump_pointers->AdapterInterfaceType = Internal;
      dump_pointers->MaximumTransferLength = 4 * 1024 * 1024; //BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;;
      dump_pointers->NumberOfPhysicalBreaks = dump_pointers->MaximumTransferLength >> PAGE_SHIFT; //BLKIF_MAX_SEGMENTS_PER_REQUEST - 1;
      dump_pointers->AlignmentMask = 0;
      dump_pointers->NumberOfAccessRanges = 1;
      dump_pointers->NumberOfBuses = 1;
      dump_pointers->Master = TRUE;
      dump_pointers->MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
      dump_pointers->MaximumNumberOfTargets = 2;

      KdPrint((__DRIVER_NAME "      Version = %d\n", dump_pointers->Version));
      KdPrint((__DRIVER_NAME "      Size = %d\n", dump_pointers->Size));
      //KdPrint((__DRIVER_NAME "      DriverName = %S\n", dump_pointers->DriverName));
      KdPrint((__DRIVER_NAME "      AdapterObject = %p\n", dump_pointers->AdapterObject));
      KdPrint((__DRIVER_NAME "      MappedRegisterBase = %d\n", dump_pointers->MappedRegisterBase));
      KdPrint((__DRIVER_NAME "      CommonBufferSize = %d\n", dump_pointers->CommonBufferSize));
      KdPrint((__DRIVER_NAME "      MiniportPrivateDumpData = %p\n", dump_pointers->MiniportPrivateDumpData));
      KdPrint((__DRIVER_NAME "      SystemIoBusNumber = %d\n", dump_pointers->SystemIoBusNumber));
      KdPrint((__DRIVER_NAME "      AdapterInterfaceType = %d\n", dump_pointers->AdapterInterfaceType));
      KdPrint((__DRIVER_NAME "      MaximumTransferLength = %d\n", dump_pointers->MaximumTransferLength));
      KdPrint((__DRIVER_NAME "      NumberOfPhysicalBreaks = %d\n", dump_pointers->NumberOfPhysicalBreaks));
      KdPrint((__DRIVER_NAME "      AlignmentMask = %d\n", dump_pointers->AlignmentMask));
      KdPrint((__DRIVER_NAME "      NumberOfAccessRanges = %d\n", dump_pointers->NumberOfAccessRanges));
      KdPrint((__DRIVER_NAME "      NumberOfBuses = %d\n", dump_pointers->NumberOfBuses));
      KdPrint((__DRIVER_NAME "      Master = %d\n", dump_pointers->Master));
      KdPrint((__DRIVER_NAME "      MapBuffers = %d\n", dump_pointers->MapBuffers));
      KdPrint((__DRIVER_NAME "      MaximumNumberOfTargets = %d\n", dump_pointers->MaximumNumberOfTargets));

      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    #endif
    case SRB_FUNCTION_SHUTDOWN:
      KdPrint((__DRIVER_NAME "     SRB_FUNCTION_SHUTDOWN %p, xvdd->shadow_free = %d\n", srb, xvdd->shadow_free));
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
      /* the path doesn't matter here - only ever one device*/
      XenVbd_ResetBus(xvdd, 0);
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      SxxxPortNotification(RequestComplete, xvdd, srb);    
      break;
    case SRB_FUNCTION_WMI:
      srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    case SRB_FUNCTION_IO_CONTROL:
      FUNCTION_MSG("SRB_FUNCTION_IO_CONTROL\n");
      sic = srb->DataBuffer;
      FUNCTION_MSG("ControlCode = %d\n", sic->ControlCode);
      srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    default:
      KdPrint((__DRIVER_NAME "     Unhandled srb->Function = %08X\n", srb->Function));
      srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
      SxxxPortNotification(RequestComplete, xvdd, srb);
      break;
    }
  }
  if (notify) {
    notify = FALSE;
    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xvdd->ring, notify);
    if (notify) {
      XnNotify(xvdd->handle, xvdd->event_channel);
    }
  }
  return;
}
