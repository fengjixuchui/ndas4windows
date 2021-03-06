/*++

Copyright (C)2002-2005 XIMETA, Inc.
All rights reserved.

--*/

#include "precomp.h"
#pragma hdrstop

//
// This is a list of all the device contexts that LPX owns,
// used while unloading.
//

LIST_ENTRY LpxDeviceList = {0,0};   // initialized for real at runtime.
PDEVICE_CONTEXT  LpxPrimaryDeviceContext = NULL; // also guarded by LpxDevicesLock

//
// And a lock that protects the global list of LPX devices and LpxPrimaryDeviceContext
//
FAST_MUTEX LpxDevicesLock;

//
// Global variables this is a copy of the path in the registry for
// configuration data.
//

UNICODE_STRING LpxRegistryPath;

//
// We need the driver object to create device context structures.
//

PDRIVER_OBJECT LpxDriverObject;

//
// A handle to be used in all provider notifications to TDI layer
//
HANDLE		 LpxProviderHandle;

//
// Global Configuration block for the driver ( no lock required )
//
PCONFIG_DATA   LpxConfig = NULL;


//
// The debugging longword, containing a bitmask as defined in LPXCONST.H.
// If a bit is set, then debugging is turned on for that component.
//

#if DBG

ULONG LpxDebug ;
 
#endif


//
// This prevents us from having a bss section
//

ULONG _setjmpexused = 0;

//
// Forward declaration of various routines used in this module.
//

NTSTATUS
DriverEntry(
	IN PDRIVER_OBJECT DriverObject,
	IN PUNICODE_STRING RegistryPath
	);

VOID
LpxUnload(
	IN PDRIVER_OBJECT DriverObject
	);

VOID
LpxFreeConfigurationInfo (
	IN PCONFIG_DATA ConfigurationInfo
	);

NTSTATUS
LpxDispatchOpenClose(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	);

NTSTATUS
LpxDispatchInternal(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	);

NTSTATUS
LpxDispatch(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	);

NTSTATUS
LpxDeviceControl(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp,
	IN PIO_STACK_LOCATION IrpSp
	);

NTSTATUS
LpxDispatchPnPPower(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp,
	IN PIO_STACK_LOCATION IrpSp
	);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,DriverEntry)
#endif


NTSTATUS
DriverEntry(
	IN PDRIVER_OBJECT DriverObject,
	IN PUNICODE_STRING RegistryPath
	)

/*++

Routine Description:

	This routine performs initialization of the NetBIOS Frames Protocol
	transport driver.  It creates the device objects for the transport
	provider and performs other driver initialization.

Arguments:

	DriverObject - Pointer to driver object created by the system.

	RegistryPath - The name of LPX's node in the registry.

Return Value:

	The function value is the final status from the initialization operation.

--*/

{
	UNICODE_STRING nameString;
	NTSTATUS status;

	ASSERT (sizeof (SHORT) == 2);

#if DBG
	DbgPrint("LPX DriverEntry %s %s\n", __DATE__, __TIME__);
	LpxDebug = 0;

//	LpxDebug |= LPX_DEBUG_DISPATCH;
	LpxDebug |= LPX_DEBUG_PNP;
//	LpxDebug |= LPX_DEBUG_ADDRESS ;
//	LpxDebug |= LPX_DEBUG_REFCOUNTS;

#endif
	LpxRegistryPath = *RegistryPath;
	LpxRegistryPath.Buffer = ExAllocatePoolWithTag(PagedPool,
						                           RegistryPath->MaximumLength,
						                           LPX_MEM_TAG_REGISTRY_PATH);

	if (LpxRegistryPath.Buffer == NULL) {
		PANIC(" Failed to allocate Registry Path!\n");
		return(STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlCopyMemory(LpxRegistryPath.Buffer, RegistryPath->Buffer,
						                        RegistryPath->MaximumLength);
	LpxDriverObject = DriverObject;
	RtlInitUnicodeString( &nameString, LPX_NAME);

	//
	// Initialize the driver object with this driver's entry points.
	//

	DriverObject->MajorFunction [IRP_MJ_CREATE] = LpxDispatchOpenClose;
	DriverObject->MajorFunction [IRP_MJ_CLOSE] = LpxDispatchOpenClose;
	DriverObject->MajorFunction [IRP_MJ_CLEANUP] = LpxDispatchOpenClose;
	DriverObject->MajorFunction [IRP_MJ_INTERNAL_DEVICE_CONTROL] = LpxDispatchInternal;
	DriverObject->MajorFunction [IRP_MJ_DEVICE_CONTROL] = LpxDispatch;

	DriverObject->MajorFunction [IRP_MJ_PNP_POWER] = LpxDispatch;

	DriverObject->DriverUnload = LpxUnload;

	//
	// Initialize the global list of devices.
	// & the lock guarding this global list
	//

	InitializeListHead (&LpxDeviceList);

	INITIALIZE_DEVICES_LIST_LOCK();

	TdiInitialize();

	status = LpxRegisterProtocol (&nameString);

	if (!NT_SUCCESS (status)) {

		//
		// No configuration info read at startup when using PNP
		//

		ExFreePool(LpxRegistryPath.Buffer);
		PANIC ("DriverEntry: RegisterProtocol with NDIS failed!\n");

		return STATUS_INSUFFICIENT_RESOURCES;

	}

	RtlInitUnicodeString( &nameString, LPX_DEVICE_NAME);

	//
	// Register as a provider with TDI
	//
	status = TdiRegisterProvider(
				&nameString,
				&LpxProviderHandle);

	if (!NT_SUCCESS (status)) {

		//
		// Deregister with the NDIS layer as TDI registration failed
		//
		LpxDeregisterProtocol();

		ExFreePool(LpxRegistryPath.Buffer);
		PANIC ("DriverEntry: RegisterProtocol with TDI failed!\n");

		return STATUS_INSUFFICIENT_RESOURCES;

	}


	status = LpxConfigureTransport(&LpxRegistryPath, &LpxConfig);

	if (!NT_SUCCESS (status)) {
		PANIC (" Failed to initialize transport, LPX binding failed.\n");
		LpxDeregisterProtocol();
		ExFreePool(LpxRegistryPath.Buffer);
		return NDIS_STATUS_RESOURCES;
	}
	LpxCreateControlDevice(LpxDriverObject);
	return(status);

}

VOID
LpxUnload(
	IN PDRIVER_OBJECT DriverObject
	)

/*++

Routine Description:

	This routine unloads the NetBIOS Frames Protocol transport driver.
	It unbinds from any NDIS drivers that are open and frees all resources
	associated with the transport. The I/O system will not call us until
	nobody above has LPX open.

Arguments:

	DriverObject - Pointer to driver object created by the system.

Return Value:

	None. When the function returns, the driver is unloaded.

--*/

{
	PDEVICE_CONTEXT DeviceContext;
	PLIST_ENTRY p;

	UNREFERENCED_PARAMETER (DriverObject);

	IF_LPXDBG (LPX_DEBUG_PNP) {
		LpxPrint0 ("ENTER LpxUnload\n");
	}

	//
	// Walk the list of device contexts.
	//

	ACQUIRE_DEVICES_LIST_LOCK();


	while (!IsListEmpty (&LpxDeviceList)) {

		// Remove an entry from list and reset its
		// links (as we might try to remove from
		// the list again - when ref goes to zero)
		p = RemoveHeadList (&LpxDeviceList);

		InitializeListHead(p);

		DeviceContext = CONTAINING_RECORD (p, DEVICE_CONTEXT, DeviceListLinkage);

		DeviceContext->State = DEVICECONTEXT_STATE_STOPPING;
		
		if(InterlockedExchange(&DeviceContext->CreateRefRemoved, TRUE) == FALSE)
		{
				InterlockedExchange(&DeviceContext->CreateRefRemoved, FALSE);
				
				RELEASE_DEVICES_LIST_LOCK();

				// Remove creation reference
				LpxDereferenceDeviceContext ("Unload", DeviceContext, DCREF_CREATION);
				
				ACQUIRE_DEVICES_LIST_LOCK();
		}else{

			// Remove creation ref if it has not already been removed
			if (InterlockedExchange(&DeviceContext->CreateRefRemoved, TRUE) == FALSE) {

				RELEASE_DEVICES_LIST_LOCK();
				// Remove creation reference
				LpxDereferenceDeviceContext ("Unload", DeviceContext, DCREF_CREATION);
				ACQUIRE_DEVICES_LIST_LOCK();
			} 
		}

	}

	RELEASE_DEVICES_LIST_LOCK();

	// Free control device context. pre-step.
	

	if(LpxControlDeviceContext)
	{
		LpxControlDeviceContext->State = DEVICECONTEXT_STATE_STOPPING;
		if(InterlockedExchange(&LpxControlDeviceContext->CreateRefRemoved, TRUE) == FALSE)
		{
				InterlockedExchange(&LpxControlDeviceContext->CreateRefRemoved, FALSE);
				// Remove creation reference
				LpxDereferenceControlContext ("Unload", LpxControlDeviceContext, DCREF_CREATION);
		}else{
			// Remove creation ref if it has not already been removed
			if (InterlockedExchange(&LpxControlDeviceContext->CreateRefRemoved, TRUE) == FALSE) {
				// Remove creation reference
				LpxDereferenceControlContext ("Unload", LpxControlDeviceContext, DCREF_CREATION);
			} 
		}
		LpxDestroyControlDevice(
			LpxControlDeviceContext
			);
		LpxControlDeviceContext = NULL;
	}
	//
	// Deregister from TDI layer as a network provider
	//
	TdiDeregisterProvider(LpxProviderHandle);

	//
	// Then remove ourselves as an NDIS protocol.
	//

	LpxDeregisterProtocol();

	//
	// Finally free any memory allocated for config info
	//
	if (LpxConfig != NULL) {

		// Free configuration block
		LpxFreeConfigurationInfo(LpxConfig);

	}

	//
	// Free memory allocated in DriverEntry for reg path
	//
	
	ExFreePool(LpxRegistryPath.Buffer);

	IF_LPXDBG (LPX_DEBUG_PNP) {
		LpxPrint0 ("LEAVE LpxUnload\n");
	}
	return;
}




VOID
LpxFreeResources (
	IN PDEVICE_CONTEXT DeviceContext
	)
/*++

Routine Description:

	This routine is called by LPX to clean up the data structures associated
	with a given DeviceContext. When this routine exits, the DeviceContext
	should be deleted as it no longer has any assocaited resources.

Arguments:

	DeviceContext - Pointer to the DeviceContext we wish to clean up.

Return Value:

	None.

--*/
{
	PLIST_ENTRY p;
	PTP_ADDRESS address;

	PTP_ADDRESS_FILE addressFile;

	//
	// Clean up address pool.
	//

	while ( !IsListEmpty (&DeviceContext->AddressPool) ) {
		p = RemoveHeadList (&DeviceContext->AddressPool);
		address = CONTAINING_RECORD (p, TP_ADDRESS, Linkage);

		LpxDeallocateAddress (DeviceContext, address);
	}

	//
	// Clean up address file pool.
	//

	while ( !IsListEmpty (&DeviceContext->AddressFilePool) ) {
		p = RemoveHeadList (&DeviceContext->AddressFilePool);
		addressFile = CONTAINING_RECORD (p, TP_ADDRESS_FILE, Linkage);

		LpxDeallocateAddressFile (DeviceContext, addressFile);
	}

	//
	// Now clean up all NDIS resources -
	// packet pools, buffers and such
	//

	//
	// Clean up Packet In Progress List.
	//
	//
	//
	//	modified by hootch 09042003
	while ( p = ExInterlockedRemoveHeadList(&DeviceContext->PacketInProgressList,
				&DeviceContext->PacketInProgressQSpinLock) ) {
		PNDIS_PACKET	Packet;
		PLPX_RESERVED	reserved;

		reserved = CONTAINING_RECORD(p, LPX_RESERVED, ListElement);
		Packet = CONTAINING_RECORD(reserved, NDIS_PACKET, ProtocolReserved);

		PacketFree(Packet);
	}

	if(DeviceContext->LpxPacketPool) {
		NdisFreePacketPool(DeviceContext->LpxPacketPool);
		DeviceContext->LpxPacketPool = NULL;
	}

	if(DeviceContext->LpxBufferPool) {
		NdisFreeBufferPool(DeviceContext->LpxBufferPool);
		DeviceContext->LpxBufferPool = NULL;
	}

	return;

}   /* LpxFreeResources */


NTSTATUS
LpxDispatch(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)

/*++

Routine Description:

	This routine is the main dispatch routine for the LPX device driver.
	It accepts an I/O Request Packet, performs the request, and then
	returns with the appropriate status.

Arguments:

	DeviceObject - Pointer to the device object for this driver.

	Irp - Pointer to the request packet representing the I/O request.

Return Value:

	The function value is the status of the operation.

--*/

{
	BOOL DeviceControlIrp = FALSE;
	NTSTATUS Status;
	PIO_STACK_LOCATION IrpSp;
	PCONTROL_CONTEXT DeviceContext;

	//
	// Check to see if LPX has been initialized; if not, don't allow any use.
	// Note that this only covers any user mode code use; kernel TDI clients
	// will fail on their creation of an endpoint.
	//

	try {
		DeviceContext = (PCONTROL_CONTEXT)DeviceObject;
		ASSERT( LpxControlDeviceContext == DeviceContext);
		if (DeviceContext->State != DEVICECONTEXT_STATE_OPEN) {
			Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
			IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
			return STATUS_INVALID_DEVICE_STATE;
		}

		// Reference the device so that it does not go away from under us
		LpxReferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);
		
	} except(EXCEPTION_EXECUTE_HANDLER) {
		Irp->IoStatus.Status = STATUS_DEVICE_DOES_NOT_EXIST;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}

	
	//
	// Make sure status information is consistent every time.
	//

	IoMarkIrpPending (Irp);
	Irp->IoStatus.Status = STATUS_PENDING;
	Irp->IoStatus.Information = 0;

	//
	// Get a pointer to the current stack location in the IRP.  This is where
	// the function codes and parameters are stored.
	//

	IrpSp = IoGetCurrentIrpStackLocation (Irp);

	//
	// Case on the function that is being performed by the requestor.  If the
	// operation is a valid one for this device, then make it look like it was
	// successfully completed, where possible.
	//


	switch (IrpSp->MajorFunction) {

		case IRP_MJ_DEVICE_CONTROL:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatch: IRP_MJ_DEVICE_CONTROL.\n");
			}

			DeviceControlIrp = TRUE;

			Status = LpxDeviceControl (DeviceObject, Irp, IrpSp);
			break;

	case IRP_MJ_PNP:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatch: IRP_MJ_PNP.\n");
			}

			Status = LpxDispatchPnPPower (DeviceObject, Irp, IrpSp);
			break;

		default:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatch: OTHER (DEFAULT).\n");
			}
			Status = STATUS_INVALID_DEVICE_REQUEST;

	} /* major function switch */

	if (Status == STATUS_PENDING) {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: request PENDING from handler.\n");
		}
	} else {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: request COMPLETED by handler.\n");
		}

		//
		// LpxDeviceControl would have completed this IRP already
		//

		if (!DeviceControlIrp)
		{
			IrpSp->Control &= ~SL_PENDING_RETURNED;
			Irp->IoStatus.Status = Status;
			IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
		}
	}

	// Remove the temp use reference on device context added above
	LpxDereferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);
	
	//
	// Return the immediate status code to the caller.
	//

	return Status;
} /* LpxDispatch */


NTSTATUS
LpxDispatchOpenClose(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)

/*++

Routine Description:

	This routine is the main dispatch routine for the LPX device driver.
	It accepts an I/O Request Packet, performs the request, and then
	returns with the appropriate status.

Arguments:

	DeviceObject - Pointer to the device object for this driver.

	Irp - Pointer to the request packet representing the I/O request.

Return Value:

	The function value is the status of the operation.

--*/

{
	PCONTROL_CONTEXT DeviceContext;
	NTSTATUS Status;
	PIO_STACK_LOCATION IrpSp;
	PFILE_FULL_EA_INFORMATION openType;
	USHORT i;
	BOOLEAN found;
	PTP_ADDRESS_FILE AddressFile;
	PTP_CONNECTION Connection;
	KIRQL irql;

	//
	// Check to see if LPX has been initialized; if not, don't allow any use.
	// Note that this only covers any user mode code use; kernel TDI clients
	// will fail on their creation of an endpoint.
	//

	try {
		DeviceContext = (PCONTROL_CONTEXT)DeviceObject;
		if (LpxControlDeviceContext != DeviceContext) {
			LpxPrint1("LpxDispatchOpenClose: Open to device object other than Lpx Control device context control: %p.\n", DeviceContext);
			Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
			IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
			return STATUS_INVALID_HANDLE;
		}

		if (DeviceContext->State != DEVICECONTEXT_STATE_OPEN) {
			Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
			IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
			return STATUS_INVALID_DEVICE_STATE;
		}

		// Reference the device so that it does not go away from under us
		LpxReferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);
		
	} except(EXCEPTION_EXECUTE_HANDLER) {
		Irp->IoStatus.Status = STATUS_DEVICE_DOES_NOT_EXIST;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}

	//
	// Make sure status information is consistent every time.
	//

	IoMarkIrpPending (Irp);
	Irp->IoStatus.Status = STATUS_PENDING;
	Irp->IoStatus.Information = 0;

	//
	// Get a pointer to the current stack location in the IRP.  This is where
	// the function codes and parameters are stored.
	//

	IrpSp = IoGetCurrentIrpStackLocation (Irp);

	//
	// Case on the function that is being performed by the requestor.  If the
	// operation is a valid one for this device, then make it look like it was
	// successfully completed, where possible.
	//


	switch (IrpSp->MajorFunction) {

	//
	// The Create function opens a transport object (either address or
	// connection).  Access checking is performed on the specified
	// address to ensure security of transport-layer addresses.
	//
	case IRP_MJ_CREATE:
		// Run at PASSIVE_LEVEL

		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: IRP_MJ_CREATE.\n");
		}

		openType =
			(PFILE_FULL_EA_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

		if (openType != NULL) {

			//
			// Address?
			//

			found = TRUE;

			if ((USHORT)openType->EaNameLength == TDI_TRANSPORT_ADDRESS_LENGTH) {
				for (i = 0; i < TDI_TRANSPORT_ADDRESS_LENGTH; i++) {
					if (openType->EaName[i] != TdiTransportAddress[i]) {
						found = FALSE;
						break;
					}
				}
			} else {
				found = FALSE;
			}

			if (found) {
				DebugPrint(2,("LPX CALL LpxOpenAddress\n"));
				Status = LpxOpenAddress (DeviceObject, Irp, IrpSp);
				break;
			}

			//
			// Connection?
			//

			found = TRUE;

			if ((USHORT)openType->EaNameLength == TDI_CONNECTION_CONTEXT_LENGTH) {
				for (i = 0; i < TDI_CONNECTION_CONTEXT_LENGTH; i++) {
					if (openType->EaName[i] != TdiConnectionContext[i]) {
						found = FALSE;
						break;
					}
				}
			}
			else {
				found = FALSE;
			}

			if (found) {
				DebugPrint(2,("LPX CALL LpxOpenConnection\n"));
				Status = LpxOpenConnection (DeviceObject, Irp, IrpSp);
				break;
			}

			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint2 ("LpxDispatchOpenClose: IRP_MJ_CREATE on invalid type, len: %3d, name: %s\n",
						    (USHORT)openType->EaNameLength, openType->EaName);
			}
			Status = STATUS_INVALID_HANDLE;
		} else {
			// Open as Control if no ea information
			
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchOpenClose: IRP_MJ_CREATE on control channel!\n");
			}

			IrpSp->FileObject->FsContext = 0;  // No parameter to pass when opening control
			IrpSp->FileObject->FsContext2 = (PVOID)LPX_FILE_TYPE_CONTROL;
			Status = STATUS_SUCCESS;
		} 

		break;

	case IRP_MJ_CLOSE:

		//
		// The Close function closes a transport endpoint, terminates
		// all outstanding transport activity on the endpoint, and unbinds
		// the endpoint from its transport address, if any.  If this
		// is the last transport endpoint bound to the address, then
		// the address is removed from the provider.
		//

		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: IRP_MJ_CLOSE.\n");
		}

		switch (PtrToUlong(IrpSp->FileObject->FsContext2)) {
		case TDI_TRANSPORT_ADDRESS_FILE:
			AddressFile = (PTP_ADDRESS_FILE)IrpSp->FileObject->FsContext;

			//
			// This creates a reference to AddressFile->Address
			// which is removed by LpxCloseAddress.
			//

			Status = LpxVerifyAddressObject(AddressFile);

			if (!NT_SUCCESS (Status)) {
				Status = STATUS_INVALID_HANDLE;
			} else {
				DebugPrint(2,("LPX CALL LpxCloseAddress\n"));
				Status = LpxCloseAddress (DeviceObject, Irp, IrpSp);
			}

			break;

		case TDI_CONNECTION_FILE:

			//
			// This is a connection
			//

			Connection = (PTP_CONNECTION)IrpSp->FileObject->FsContext;

			Status = LpxVerifyConnectionObject (Connection);
			if (NT_SUCCESS (Status)) {
				DebugPrint(3,("LPX CALL LpxCloseConnection\n"));
				Status = LpxCloseConnection (DeviceObject, Irp, IrpSp);
				LpxDereferenceConnection ("Temporary Use",Connection, CREF_BY_ID);

			}

			break;

		case LPX_FILE_TYPE_CONTROL:

			//
			// this always succeeds
			//

			Status = STATUS_SUCCESS;
			break;

		default:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint1 ("LpxDispatch: IRP_MJ_CLOSE on unknown file type %p.\n",
					IrpSp->FileObject->FsContext2);
			}

			Status = STATUS_INVALID_HANDLE;
		}

		break;

	case IRP_MJ_CLEANUP:

		//
		// Handle the two stage IRP for a file close operation. When the first
		// stage hits, run down all activity on the object of interest. This
		// do everything to it but remove the creation hold. Then, when the
		// CLOSE irp hits, actually close the object.
		//

		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: IRP_MJ_CLEANUP.\n");
		}

		switch (PtrToUlong(IrpSp->FileObject->FsContext2)) {
		case TDI_TRANSPORT_ADDRESS_FILE:
			AddressFile = (PTP_ADDRESS_FILE)IrpSp->FileObject->FsContext;
			Status = LpxVerifyAddressObject(AddressFile);
			if (!NT_SUCCESS (Status)) {

				Status = STATUS_INVALID_HANDLE;

			} else {

				LpxStopAddressFile (AddressFile, AddressFile->Address);
				LpxDereferenceAddress ("IRP_MJ_CLEANUP", AddressFile->Address, AREF_VERIFY);
				Status = STATUS_SUCCESS;
			}

			break;

		case TDI_CONNECTION_FILE:
			Connection = (PTP_CONNECTION)IrpSp->FileObject->FsContext;
			Status = LpxVerifyConnectionObject (Connection);
			if (NT_SUCCESS (Status)) {
				ACQUIRE_SPIN_LOCK (&Connection->TpConnectionSpinLock, &irql);
				Connection->Flags2 |= CONNECTION_FLAGS2_STOPPING;
				RELEASE_SPIN_LOCK (&Connection->TpConnectionSpinLock, irql);		    
				Status = STATUS_SUCCESS;
				LpxDereferenceConnection ("Temporary Use",Connection, CREF_BY_ID);
			}

			break;

		case LPX_FILE_TYPE_CONTROL:
			// nothing to do
			Status = STATUS_SUCCESS;
			break;

		default:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint1 ("LpxDispatch: IRP_MJ_CLEANUP on unknown file type %p.\n",
					IrpSp->FileObject->FsContext2);
			}

			Status = STATUS_INVALID_HANDLE;
		}

		break;

	default:
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: OTHER (DEFAULT).\n");
		}

		Status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	} /* major function switch */

	if (Status == STATUS_PENDING) {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: request PENDING from handler.\n");
		}
	} else {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatch: request COMPLETED by handler.\n");
		}

		IrpSp->Control &= ~SL_PENDING_RETURNED;
		Irp->IoStatus.Status = Status;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
	}

	// Remove the temp use reference on device context added above
	LpxDereferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);

	//
	// Return the immediate status code to the caller.
	//

	return Status;
} /* LpxDispatchOpenClose */


NTSTATUS
LpxDeviceControl(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp,
	IN PIO_STACK_LOCATION IrpSp
	)

/*++

Routine Description:

	This routine dispatches TDI request types to different handlers based
	on the minor IOCTL function code in the IRP's current stack location.
	In addition to cracking the minor function code, this routine also
	reaches into the IRP and passes the packetized parameters stored there
	as parameters to the various TDI request handlers so that they are
	not IRP-dependent.

Arguments:

	DeviceObject - Pointer to the device object for this driver.

	Irp - Pointer to the request packet representing the I/O request.

	IrpSp - Pointer to current IRP stack frame.

Return Value:

	The function value is the status of the operation.

--*/

{
	BOOL InternalIrp = FALSE;
	NTSTATUS Status;
	PCONTROL_CONTEXT DeviceContext = (PCONTROL_CONTEXT)DeviceObject;
		ASSERT( LpxControlDeviceContext == DeviceContext) ;
	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		LpxPrint0 ("LpxDeviceControl: Entered.\n");
	}

	//
	// Branch to the appropriate request handler.  Preliminary checking of
	// the size of the request block is performed here so that it is known
	// in the handlers that the minimum input parameters are readable.  It
	// is *not* determined here whether variable length input fields are
	// passed correctly;this is a check which must be made within each routine.
	//

	switch (IrpSp->Parameters.DeviceIoControl.IoControlCode) {

		case IOCTL_LPX_GET_VERSION: {
			LPXDRV_VER	version;
			PVOID		outputBuffer;
			ULONG		outputBufferLength;

			outputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			outputBuffer = Irp->UserBuffer;
			DebugPrint(3, ("LPX: IOCTL_PX_GET_VERSION, outputBufferLength= %d, outputBuffer = %p\n", 
				outputBufferLength, outputBuffer));

			version.VersionMajor = VER_FILEMAJORVERSION;
			version.VersionMinor = VER_FILEMINORVERSION;
			version.VersionBuild = VER_FILEBUILD;
			version.VersionPrivate = VER_FILEBUILD_QFE;

			Irp->IoStatus.Information = sizeof(LPXDRV_VER);
			Status = STATUS_SUCCESS;

			try {

				RtlCopyMemory(outputBuffer, &version, sizeof(LPXDRV_VER));

			} except (EXCEPTION_EXECUTE_HANDLER) {

				Status = GetExceptionCode();
				Irp->IoStatus.Information = 0;
			}
			break;
		}

		case IOCTL_LPX_QUERY_ADDRESS_LIST:
		{
			PVOID			        outputBuffer;
			ULONG			        outputBufferLength;
			SOCKETLPX_ADDRESS_LIST	socketLpxAddressList;

			PLIST_ENTRY		listHead;
			PLIST_ENTRY		thisEntry;

			outputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			outputBuffer = Irp->UserBuffer;

			DebugPrint(3, ("LPX: IOCTL_LPX_QUERY_ADDRESS_LIST, outputBufferLength= %d, outputBuffer = %p\n", 
				outputBufferLength, outputBuffer));

			if(outputBufferLength < sizeof(SOCKETLPX_ADDRESS_LIST)) {
				Status = STATUS_INVALID_PARAMETER;
				Irp->IoStatus.Information = 0;
				break;
			}

			ACQUIRE_DEVICES_LIST_LOCK();

			RtlZeroMemory(
				&socketLpxAddressList,
				sizeof(SOCKETLPX_ADDRESS_LIST)
				);

			socketLpxAddressList.iAddressCount = 0;

			if(IsListEmpty (&LpxDeviceList)) {
				RELEASE_DEVICES_LIST_LOCK();

				RtlCopyMemory(
					outputBuffer,
					&socketLpxAddressList,
					sizeof(SOCKETLPX_ADDRESS_LIST)
					);

				Irp->IoStatus.Information = sizeof(SOCKETLPX_ADDRESS_LIST);
				Status = STATUS_SUCCESS;
				break;
			}

			listHead = &LpxDeviceList;
			for(thisEntry = listHead->Flink;
				thisEntry != listHead;
				thisEntry = thisEntry->Flink)
			{
				PDEVICE_CONTEXT deviceContext;
	
				deviceContext = CONTAINING_RECORD (thisEntry, DEVICE_CONTEXT, DeviceListLinkage);
				if(deviceContext->CreateRefRemoved == FALSE && deviceContext->bDeviceInit == TRUE)
				{
					socketLpxAddressList.SocketLpx[socketLpxAddressList.iAddressCount].sin_family 
						= TDI_ADDRESS_TYPE_LPX;
					socketLpxAddressList.SocketLpx[socketLpxAddressList.iAddressCount].LpxAddress.Port = 0;
					RtlCopyMemory(
						&socketLpxAddressList.SocketLpx[socketLpxAddressList.iAddressCount].LpxAddress.Node,
						deviceContext->LocalAddress.Address,
						HARDWARE_ADDRESS_LENGTH
						);
						
					socketLpxAddressList.iAddressCount++;
					ASSERT(socketLpxAddressList.iAddressCount <= MAX_SOCKETLPX_INTERFACE);
					if(socketLpxAddressList.iAddressCount == MAX_SOCKETLPX_INTERFACE)
						break;
				}
			}

			RELEASE_DEVICES_LIST_LOCK();
	
			RtlCopyMemory(
				outputBuffer,
				&socketLpxAddressList,
				sizeof(SOCKETLPX_ADDRESS_LIST)
				);

			Irp->IoStatus.Information = sizeof(SOCKETLPX_ADDRESS_LIST);
			Status = STATUS_SUCCESS;

			break;
		}
		
		case IOCTL_LPX_SET_INFORMATION_EX:
					Status = STATUS_NOT_IMPLEMENTED;
			break;

#if 1
		case IOCTL_LPX_GET_RX_DROP_RATE:
			{
				ULONG	outputBufferLength;
				PULONG	pulData;

				outputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
				pulData = (PULONG)Irp->AssociatedIrp.SystemBuffer;

				if(outputBufferLength < sizeof(ULONG)) {
					Status = STATUS_INVALID_BUFFER_SIZE;
					break;
				}
				
				*pulData = ulRxPacketDropRate;

				DebugPrint(1, ("[LPX]IOCTL_LPX_GET_RX_DROP_RATE: %d\n", *pulData));

				Irp->IoStatus.Information = sizeof(ULONG);
				Status = STATUS_SUCCESS;

			}
			break;

		case IOCTL_LPX_SET_RX_DROP_RATE:
			{
				ULONG	inputBufferLength;
				PULONG	pulData;
//					DbgBreakPoint();

				inputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
				pulData = (PULONG)Irp->AssociatedIrp.SystemBuffer;

				if(inputBufferLength < sizeof(ULONG)) {
					Status = STATUS_INVALID_BUFFER_SIZE;
					break;
				}

				DebugPrint(1, ("[LPX]IOCTL_LPX_SET_RX_DROP_RATE: %d -> %d\n", ulRxPacketDropRate, *pulData));

				ulRxPacketDropRate = *pulData;

				Irp->IoStatus.Information = 0;
				Status = STATUS_SUCCESS;
			}
			break;
		case IOCTL_LPX_GET_TX_DROP_RATE:
			{
				ULONG	outputBufferLength;
				PULONG	pulData;

				outputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
				pulData = (PULONG)Irp->AssociatedIrp.SystemBuffer;

				if(outputBufferLength < sizeof(ULONG)) {
					Status = STATUS_INVALID_BUFFER_SIZE;
					break;
				}
				
				*pulData = ulTxPacketDropRate;

				DebugPrint(1, ("[LPX]IOCTL_LPX_GET_TX_DROP_RATE: %d\n", *pulData));

				Irp->IoStatus.Information = sizeof(ULONG);
				Status = STATUS_SUCCESS;

			}
			break;

		case IOCTL_LPX_SET_TX_DROP_RATE:
			{
				ULONG	inputBufferLength;
				PULONG	pulData;
//					DbgBreakPoint();

				inputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
				pulData = (PULONG)Irp->AssociatedIrp.SystemBuffer;

				if(inputBufferLength < sizeof(ULONG)) {
					Status = STATUS_INVALID_BUFFER_SIZE;
					break;
				}

				DebugPrint(1, ("[LPX]IOCTL_LPX_SET_TX_DROP_RATE: %d -> %d\n", ulTxPacketDropRate, *pulData));

				ulTxPacketDropRate = *pulData;

				Irp->IoStatus.Information = 0;
				Status = STATUS_SUCCESS;
			}
			break;
#endif

		default:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDeviceControl: invalid request type.\n");
			}

			//
			// Convert the user call to the proper internal device call.
			//

			Status = TdiMapUserRequest (DeviceObject, Irp, IrpSp);

			if (Status == STATUS_SUCCESS) {

				//
				// If TdiMapUserRequest returns SUCCESS then the IRP
				// has been converted into an IRP_MJ_INTERNAL_DEVICE_CONTROL
				// IRP, so we dispatch it as usual. The IRP will be
				// completed by this call to LpxDispatchInternal, so we dont
				//

				InternalIrp = TRUE;

				Status = LpxDispatchInternal (DeviceObject, Irp);
			}
			break;
	}

	//
	// If this IRP got converted to an internal IRP,
	// it will be completed by LpxDispatchInternal.
	//

	if ((!InternalIrp) && (Status != STATUS_PENDING))
	{
		IrpSp->Control &= ~SL_PENDING_RETURNED;
		Irp->IoStatus.Status = Status;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
	}

	return Status;
} /* LpxDeviceControl */

NTSTATUS
LpxDispatchPnPPower(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp,
	IN PIO_STACK_LOCATION IrpSp
	)

/*++

Routine Description:

	This routine dispatches PnP request types to different handlers based
	on the minor IOCTL function code in the IRP's current stack location.

Arguments:

	DeviceObject - Pointer to the device object for this driver.

	Irp - Pointer to the request packet representing the I/O request.

	IrpSp - Pointer to current IRP stack frame.

Return Value:

	The function value is the status of the operation.

--*/

{
	PDEVICE_RELATIONS DeviceRelations = NULL;
	PTP_CONNECTION Connection;
	PVOID PnPContext;
	NTSTATUS Status;

	UNREFERENCED_PARAMETER(DeviceObject) ;

	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		LpxPrint0 ("LpxDispatchPnPPower: Entered.\n");
	}

	Status = STATUS_INVALID_DEVICE_REQUEST;

	switch (IrpSp->MinorFunction) {

	case IRP_MN_QUERY_DEVICE_RELATIONS:

	  if (IrpSp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation){

		switch (PtrToUlong(IrpSp->FileObject->FsContext2))
		{
		case TDI_CONNECTION_FILE:

			// Get the connection object and verify
			Connection = IrpSp->FileObject->FsContext;

			//
			// This adds a connection reference of type BY_ID if successful.
			//

			Status = LpxVerifyConnectionObject (Connection);

			if (NT_SUCCESS (Status)) {

				//
				// Get the PDO associated with conn's device object
				//
				// sjcho: Return DEVICE_CONTEXT's PnPContext ??
				PnPContext = NULL; 

				if (PnPContext) {

					DeviceRelations = 
						ExAllocatePoolWithTag(NonPagedPool,
						                      sizeof(DEVICE_RELATIONS),
						                      LPX_MEM_TAG_DEVICE_PDO);
					if (DeviceRelations) {

						//
						// TargetDeviceRelation allows exactly 1 PDO. fill it.
						//
						DeviceRelations->Count = 1;
						DeviceRelations->Objects[0] = PnPContext;
						ObReferenceObject(PnPContext);

					} else {
						Status = STATUS_NO_MEMORY;
					}
				} else {
					Status = STATUS_INVALID_DEVICE_STATE;
				}
			
				LpxDereferenceConnection ("Temp Rel", Connection, CREF_BY_ID);
			}
			break;
			
		case TDI_TRANSPORT_ADDRESS_FILE:

			Status = STATUS_UNSUCCESSFUL;
			break;
		}
	  }
	}

	//
	// Invoker of this irp will free the information buffer.
	//

	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = (ULONG_PTR) DeviceRelations;

	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		LpxPrint1 ("LpxDispatchPnPPower: exiting, status: %lx\n",Status);
	}

	return Status;
} /* LpxDispatchPnPPower */


NTSTATUS
LpxDispatchInternal (
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)

/*++

Routine Description:

	This routine dispatches TDI request types to different handlers based
	on the minor IOCTL function code in the IRP's current stack location.
	In addition to cracking the minor function code, this routine also
	reaches into the IRP and passes the packetized parameters stored there
	as parameters to the various TDI request handlers so that they are
	not IRP-dependent.

Arguments:

	DeviceObject - Pointer to the device object for this driver.

	Irp - Pointer to the request packet representing the I/O request.

Return Value:

	The function value is the status of the operation.

--*/

{
	NTSTATUS Status;
	PCONTROL_CONTEXT DeviceContext;
	PIO_STACK_LOCATION IrpSp;
#if DBG
	KIRQL IrqlOnEnter = KeGetCurrentIrql();
#endif


	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		LpxPrint0 ("LpxInternalDeviceControl: Entered.\n");
	}

	//
	// Get a pointer to the current stack location in the IRP.  This is where
	// the function codes and parameters are stored.
	//

	IrpSp = IoGetCurrentIrpStackLocation (Irp);

	DeviceContext = (PCONTROL_CONTEXT)DeviceObject;
	ASSERT( LpxControlDeviceContext == DeviceContext) ;
	try {
		if (DeviceContext->State != DEVICECONTEXT_STATE_OPEN) {
			Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
			IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
			return STATUS_INVALID_DEVICE_STATE;
		}
	
		// Reference the device so that it does not go away from under us
		LpxReferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);
		
	} except(EXCEPTION_EXECUTE_HANDLER) {
		Irp->IoStatus.Status = STATUS_DEVICE_DOES_NOT_EXIST;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}

	//
	// Make sure status information is consistent every time.
	//

	IoMarkIrpPending (Irp);
	Irp->IoStatus.Status = STATUS_PENDING;
	Irp->IoStatus.Information = 0;


	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		{
			PULONG Temp=(PULONG)&IrpSp->Parameters;
			LpxPrint5 ("Got IrpSp %lx %lx %lx %lx %lx\n", *(Temp++),  *(Temp++),
				*(Temp++), *(Temp++), *(Temp++));
		}
	}

	//
	// Branch to the appropriate request handler.  Preliminary checking of
	// the size of the request block is performed here so that it is known
	// in the handlers that the minimum input parameters are readable.  It
	// is *not* determined here whether variable length input fields are
	// passed correctly; this is a check which must be made within each routine.
	//

	switch (IrpSp->MinorFunction) {

		case TDI_ACCEPT:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiAccept request.\n");
			}

			Status = LpxTdiAccept (Irp);
			break;

		case TDI_ACTION:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiAction request.\n");
			}
			Status = STATUS_NOT_SUPPORTED; //LpxTdiAction
			break;

		case TDI_ASSOCIATE_ADDRESS:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiAccept request.\n");
			}
			DebugPrint(2,("LpxDispatchInternal: LpxTdiAssociateAddress request.\n"));
			Status = LpxTdiAssociateAddress (Irp);
			break;

		case TDI_DISASSOCIATE_ADDRESS:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiDisassociateAddress request.\n");
			}
			DebugPrint(3,("LpxDispatchInternal: LpxTdiDisassociateAddress request.\n"));
			Status = LpxTdiDisassociateAddress (Irp);
			break;

		case TDI_CONNECT:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiConnect request\n");
			}

			Status = LpxTdiConnect (Irp);
			break;

		case TDI_DISCONNECT:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiDisconnect request.\n");
			}

			Status = LpxTdiDisconnect (Irp);
			break;

		case TDI_LISTEN:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiListen request.\n");
			}

			Status = LpxTdiListen (Irp);
			break;

		case TDI_QUERY_INFORMATION:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiQueryInformation request.\n");
			}

			Status = LpxTdiQueryInformation (DeviceContext, Irp);
			break;

		case TDI_RECEIVE:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiReceive request.\n");
			}

			Status =  LpxTdiReceive (Irp);
			break;

		case TDI_RECEIVE_DATAGRAM:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiReceiveDatagram request.\n");
			}

			Status =  LpxTdiReceiveDatagram (Irp);
			break;

		case TDI_SEND:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiSend request.\n");
			}

			Status =  LpxTdiSend (Irp);
			break;

		case TDI_SEND_DATAGRAM:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiSendDatagram request.\n");
		   }

		   Status = LpxTdiSendDatagram (Irp);
			break;

		case TDI_SET_EVENT_HANDLER:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiSetEventHandler request.\n");
			}

			//
			// Because this request will enable direct callouts from the
			// transport provider at DISPATCH_LEVEL to a client-specified
			// routine, this request is only valid in kernel mode, denying
			// access to this request in user mode.
			//

			Status = LpxTdiSetEventHandler (Irp);
			break;

		case TDI_SET_INFORMATION:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint0 ("LpxDispatchInternal: TdiSetInformation request.\n");
			}

			Status = LpxTdiSetInformation (Irp);
			break;

		//
		// Something we don't know about was submitted.
		//

		default:
			IF_LPXDBG (LPX_DEBUG_DISPATCH) {
				LpxPrint1 ("LpxDispatchInternal: invalid request type %lx\n",
				IrpSp->MinorFunction);
			}
			Status = STATUS_INVALID_DEVICE_REQUEST;

			break;
	}

	if (Status == STATUS_PENDING) {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatchInternal: request PENDING from handler.\n");
		}
	} else {
		IF_LPXDBG (LPX_DEBUG_DISPATCH) {
			LpxPrint0 ("LpxDispatchInternal: request COMPLETED by handler.\n");
		}

		IrpSp->Control &= ~SL_PENDING_RETURNED;
		Irp->IoStatus.Status = Status;
		IoCompleteRequest (Irp, IO_NETWORK_INCREMENT);
	}

	IF_LPXDBG (LPX_DEBUG_DISPATCH) {
		LpxPrint1 ("LpxDispatchInternal: exiting, status: %lx\n",Status);
	}

	// Remove the temp use reference on device context added above
	LpxDereferenceControlContext ("Temp Use Ref", DeviceContext, DCREF_TEMP_USE);

	//
	// Return the immediate status code to the caller.
	//

#if DBG
	ASSERT (KeGetCurrentIrql() == IrqlOnEnter);
#endif
	return Status;
} /* LpxDispatchInternal */

ULONG
LpxInitializeOneDeviceContext(
						        OUT PNDIS_STATUS NdisStatus,
						        IN PDRIVER_OBJECT DriverObject,
						        IN PCONFIG_DATA LpxConfig,
						        IN PUNICODE_STRING BindName,
						        IN PUNICODE_STRING ExportName,
						        IN PVOID SystemSpecific1,
						        IN PVOID SystemSpecific2
						     )
/*++

Routine Description:

	This routine creates and initializes one lpx device context.  In order to
	do this it must successfully open and bind to the adapter described by
	lpxconfig->names[adapterindex].

Arguments:

	NdisStatus   - The outputted status of the operations.

	DriverObject - the lpx driver object.

	LpxConfig	- the transport configuration information from the registry.

	SystemSpecific1 - SystemSpecific1 argument to ProtocolBindAdapter

	SystemSpecific2 - SystemSpecific2 argument to ProtocolBindAdapter

Return Value:

	The number of successful binds.

--*/

{
	ULONG i;
	PDEVICE_CONTEXT DeviceContext;
//	 PTP_CONNECTION Connection;
	PTP_ADDRESS_FILE AddressFile;
	PTP_ADDRESS Address;
	NTSTATUS status;
	PDEVICE_OBJECT DeviceObject;
	UNICODE_STRING DeviceString;
	UCHAR PermAddr[sizeof(TA_ADDRESS)+TDI_ADDRESS_LENGTH_LPX];
	PTA_ADDRESS pAddress = (PTA_ADDRESS)PermAddr;
	struct {
		TDI_PNP_CONTEXT tdiPnPContextHeader;
		PVOID		   tdiPnPContextTrailer;
	} tdiPnPContext2;
	pAddress->AddressLength = TDI_ADDRESS_LENGTH_LPX;
	pAddress->AddressType = TDI_ADDRESS_TYPE_LPX;


	//
	// Loop through all the adapters that are in the configuration
	// information structure. Allocate a device object for each
	// one that we find.
	//

	status = LpxCreateDeviceContext(
						            DriverObject,
						            ExportName,
						            &DeviceContext
						           );

	if (!NT_SUCCESS (status)) {

		IF_LPXDBG (LPX_DEBUG_PNP) {
			LpxPrint2 ("LpxCreateDeviceContext for %S returned error %08x\n",
						    ExportName->Buffer, status);
		}

		//
		// First check if we already have an object with this name
		// This is because a previous unbind was not done properly.
		//

		if (status == STATUS_OBJECT_NAME_COLLISION) {

			// See if we can reuse the binding and device name

			LpxReInitializeDeviceContext(
						                 &status,
						                 DriverObject,
						                 LpxConfig,
						                 BindName,
						                 ExportName,
						                 SystemSpecific1,
						                 SystemSpecific2
						                );

			if (status == STATUS_NOT_FOUND)
			{
				// Must have got deleted in the meantime

				return LpxInitializeOneDeviceContext(
						                             NdisStatus,
						                             DriverObject,
						                             LpxConfig,
						                             BindName,
						                             ExportName,
						                             SystemSpecific1,
						                             SystemSpecific2
						                            );
			}
		}

		*NdisStatus = status;

		if (!NT_SUCCESS (status))
		{
			// error 
			return(0);
		}
		return(1);
	}

  
	//
	// Initialize our counter that records memory usage.
	//
 
	DeviceContext->MaxConnections = LpxConfig->MaxConnections;
	DeviceContext->MaxAddressFiles = LpxConfig->MaxAddressFiles;
	DeviceContext->MaxAddresses = LpxConfig->MaxAddresses;

	//
	// Now fire up NDIS so this adapter talks
	//

	status = LpxInitializeNdis (DeviceContext,
						        LpxConfig,
						        BindName);

	if (!NT_SUCCESS (status)) {

		if (InterlockedExchange(&DeviceContext->CreateRefRemoved, TRUE) == FALSE) {
			LpxDereferenceDeviceContext ("Initialize NDIS failed", DeviceContext, DCREF_CREATION);
		}
		
		*NdisStatus = status;
		return(0);

	}

#if 0
	DbgPrint("Opened %S as %S\n", &LpxConfig->Names[j], &nameString);
#endif

	IF_LPXDBG (LPX_DEBUG_RESOURCE) {
		LpxPrint6 ("LpxInitialize: NDIS returned: %x %x %x %x %x %x as local address.\n",
			DeviceContext->LocalAddress.Address[0],
			DeviceContext->LocalAddress.Address[1],
			DeviceContext->LocalAddress.Address[2],
			DeviceContext->LocalAddress.Address[3],
			DeviceContext->LocalAddress.Address[4],
			DeviceContext->LocalAddress.Address[5]);
	}

	//
	// Initialize our provider information structure; since it
	// doesn't change, we just keep it around and copy it to
	// whoever requests it.
	//


	LpxReturnMaxDataSize(
		DeviceContext->MaxSendPacketSize,
		&DeviceContext->MaxUserData,
		&DeviceContext->MaxJumboUserData
		);

	IF_LPXDBG (LPX_DEBUG_RESOURCE) {
		LpxPrint0 ("LPXDRVR: allocating AddressFiles.\n");
	}
	for (i=0; i<LpxConfig->InitAddressFiles; i++) {

		LpxAllocateAddressFile (DeviceContext, &AddressFile);

		if (AddressFile == NULL) {
			PANIC ("LpxInitialize:  insufficient memory to allocate Address Files.\n");
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		InsertTailList (&DeviceContext->AddressFilePool, &AddressFile->Linkage);
	}

	DeviceContext->AddressFileInitAllocated = LpxConfig->InitAddressFiles;
	DeviceContext->AddressFileMaxAllocated = LpxConfig->MaxAddressFiles;

	IF_LPXDBG (LPX_DEBUG_DYNAMIC) {
		LpxPrint1("%d address files\n", LpxConfig->InitAddressFiles );
	}


	IF_LPXDBG (LPX_DEBUG_RESOURCE) {
		LpxPrint0 ("LPXDRVR: allocating addresses.\n");
	}
	for (i=0; i<LpxConfig->InitAddresses; i++) {

		LpxAllocateAddress (DeviceContext, &Address);
		if (Address == NULL) {
			PANIC ("LpxInitialize:  insufficient memory to allocate addresses.\n");
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		InsertTailList (&DeviceContext->AddressPool, &Address->Linkage);
	}

	DeviceContext->AddressInitAllocated = LpxConfig->InitAddresses;
	DeviceContext->AddressMaxAllocated = LpxConfig->MaxAddresses;

	IF_LPXDBG (LPX_DEBUG_DYNAMIC) {
		LpxPrint1("%d addresses\n", LpxConfig->InitAddresses );
	}

	// Store away the PDO for the underlying object
	DeviceContext->PnPContext = SystemSpecific2;

	DeviceContext->State = DEVICECONTEXT_STATE_OPEN;

	//
	// Now link the device into the global list.
	//

	ACQUIRE_DEVICES_LIST_LOCK();
	InsertTailList (&LpxDeviceList, &DeviceContext->DeviceListLinkage);
	RELEASE_DEVICES_LIST_LOCK();

	DeviceObject = (PDEVICE_OBJECT) DeviceContext;
	DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	RtlInitUnicodeString(&DeviceString, DeviceContext->DeviceName);

	IF_LPXDBG (LPX_DEBUG_PNP) {
		LpxPrint1 ("TdiRegisterDeviceObject for %S\n", DeviceString.Buffer);
	}

	status = TdiRegisterDeviceObject(&DeviceString,
						             &DeviceContext->TdiDeviceHandle);

	if (!NT_SUCCESS (status)) {
		ACQUIRE_DEVICES_LIST_LOCK();
		RemoveEntryList(&DeviceContext->DeviceListLinkage);
		RELEASE_DEVICES_LIST_LOCK();
		goto cleanup;
	}

	tdiPnPContext2.tdiPnPContextHeader.ContextSize = sizeof(PVOID);
	tdiPnPContext2.tdiPnPContextHeader.ContextType = TDI_PNP_CONTEXT_TYPE_PDO;
	*(PVOID UNALIGNED *) &tdiPnPContext2.tdiPnPContextHeader.ContextData = SystemSpecific2;

#ifdef __LPX__
{ 
	PTDI_ADDRESS_LPX LpxAddress = (PTDI_ADDRESS_LPX)pAddress->Address;
	RtlZeroMemory(LpxAddress, sizeof(TDI_ADDRESS_LPX));
	RtlCopyMemory (
		&LpxAddress->Node,
		DeviceContext->LocalAddress.Address,
		HARDWARE_ADDRESS_LENGTH
		);
} 
#endif

	status = TdiRegisterNetAddress(pAddress,
						           &DeviceString,
						           (TDI_PNP_CONTEXT *) &tdiPnPContext2,
						           &DeviceContext->ReservedAddressHandle);

	if (!NT_SUCCESS (status)) {
		RemoveEntryList(&DeviceContext->DeviceListLinkage);
		goto cleanup;
	}

	LpxReferenceDeviceContext ("Load Succeeded", DeviceContext, DCREF_CREATION);

	*NdisStatus = NDIS_STATUS_SUCCESS;

	return(1);

cleanup:

	 //
	// Cleanup whatever device context we were initializing
	// when we failed.
	//
	*NdisStatus = status;
	ASSERT(status != STATUS_SUCCESS);
	
	if (InterlockedExchange(&DeviceContext->CreateRefRemoved, TRUE) == FALSE) {
		// Remove creation reference
		LpxDereferenceDeviceContext ("Load failed", DeviceContext, DCREF_CREATION);
	}


	return (0);
}


VOID
LpxReInitializeDeviceContext(
						        OUT PNDIS_STATUS NdisStatus,
						        IN PDRIVER_OBJECT DriverObject,
						        IN PCONFIG_DATA LpxConfig,
						        IN PUNICODE_STRING BindName,
						        IN PUNICODE_STRING ExportName,
						        IN PVOID SystemSpecific1,
						        IN PVOID SystemSpecific2
						    )
/*++

Routine Description:

	This routine re-initializes an existing lpx device context. In order to
	do this, we need to undo whatever is done in the Unbind handler exposed
	to NDIS - recreate the NDIS binding, and re-start the LPX timer system.

Arguments:

	NdisStatus   - The outputted status of the operations.

	DriverObject - the driver object.

	LpxConfig	- the transport configuration information from the registry.

	SystemSpecific1 - SystemSpecific1 argument to ProtocolBindAdapter

	SystemSpecific2 - SystemSpecific2 argument to ProtocolBindAdapter

Return Value:

	None

--*/

{
	PDEVICE_CONTEXT DeviceContext;
//	KIRQL oldIrql;
	PLIST_ENTRY p;
	NTSTATUS status;
	UNICODE_STRING DeviceString;
	UCHAR PermAddr[sizeof(TA_ADDRESS)+TDI_ADDRESS_LENGTH_LPX];
	PTA_ADDRESS pAddress = (PTA_ADDRESS)PermAddr;
	struct {
		TDI_PNP_CONTEXT tdiPnPContextHeader;
		PVOID		   tdiPnPContextTrailer;
	} tdiPnPContext2;
	UNREFERENCED_PARAMETER(DriverObject) ;
	UNREFERENCED_PARAMETER(SystemSpecific1) ;
	UNREFERENCED_PARAMETER(SystemSpecific2) ;

	IF_LPXDBG (LPX_DEBUG_PNP) {
		LpxPrint1 ("ENTER LpxReInitializeDeviceContext for %S\n",
						ExportName->Buffer);
	}

	//
	// Search the list of LPX devices for a matching device name
	//
	
	ACQUIRE_DEVICES_LIST_LOCK();

	for (p = LpxDeviceList.Flink ; p != &LpxDeviceList; p = p->Flink)
	{
		DeviceContext = CONTAINING_RECORD (p, DEVICE_CONTEXT, DeviceListLinkage);

		RtlInitUnicodeString(&DeviceString, DeviceContext->DeviceName);

		if (NdisEqualString(&DeviceString, ExportName, TRUE)) {
						    
			// This has to be a rebind - otherwise something wrong

			ASSERT(DeviceContext->CreateRefRemoved == TRUE);

			// Reference within lock so that it is not cleaned up

			LpxReferenceDeviceContext ("Reload Temp Use", DeviceContext, DCREF_TEMP_USE);
			DebugPrint(0,("[LPX]Found diabled Device Context for resuse !!  %p\n", DeviceContext));
			break;
		}
	}

	if (p == &LpxDeviceList)
	{
		RELEASE_DEVICES_LIST_LOCK();
		IF_LPXDBG (LPX_DEBUG_PNP) {
			LpxPrint2 ("LEAVE LpxReInitializeDeviceContext for %S with Status %08x\n",
						    ExportName->Buffer,
						    STATUS_NOT_FOUND);
		}

		*NdisStatus = STATUS_NOT_FOUND;

		return;
	}
	RELEASE_DEVICES_LIST_LOCK();

	//
	// Fire up NDIS again so this adapter talks
	//

	status = LpxInitializeNdis (DeviceContext,
						        LpxConfig,
						        BindName);

	if (!NT_SUCCESS (status)) {
		goto Cleanup;
	}

	LpxReturnMaxDataSize(
		DeviceContext->MaxSendPacketSize,
		&DeviceContext->MaxUserData,
		&DeviceContext->MaxJumboUserData
		);

	// Store away the PDO for the underlying object
	DeviceContext->PnPContext = SystemSpecific2;

	DeviceContext->State = DEVICECONTEXT_STATE_OPEN;

	//
	// Re-Indicate to TDI that new binding has arrived
	//

	status = TdiRegisterDeviceObject(&DeviceString,
						             &DeviceContext->TdiDeviceHandle);

	if (!NT_SUCCESS (status)) {
		goto Cleanup;
	}


	pAddress->AddressLength = TDI_ADDRESS_LENGTH_LPX;
	pAddress->AddressType = TDI_ADDRESS_TYPE_LPX;

	tdiPnPContext2.tdiPnPContextHeader.ContextSize = sizeof(PVOID);
	tdiPnPContext2.tdiPnPContextHeader.ContextType = TDI_PNP_CONTEXT_TYPE_PDO;
	*(PVOID UNALIGNED *) &tdiPnPContext2.tdiPnPContextHeader.ContextData = SystemSpecific2;

#ifdef __LPX__
{ 
	PTDI_ADDRESS_LPX LpxAddress = (PTDI_ADDRESS_LPX)pAddress->Address;
	RtlZeroMemory(LpxAddress, sizeof(TDI_ADDRESS_LPX));
	RtlCopyMemory (
		&LpxAddress->Node,
		DeviceContext->LocalAddress.Address,
		HARDWARE_ADDRESS_LENGTH
		);
} 
#endif

	status = TdiRegisterNetAddress(pAddress,
						           &DeviceString,
						           (TDI_PNP_CONTEXT *) &tdiPnPContext2,
						           &DeviceContext->ReservedAddressHandle);

	if (!NT_SUCCESS (status)) {
		goto Cleanup;
	}

	// Put the creation reference back again
	LpxReferenceDeviceContext ("Reload Succeeded", DeviceContext, DCREF_CREATION);

	DeviceContext->CreateRefRemoved = FALSE;

	status = NDIS_STATUS_SUCCESS;

Cleanup:

	LpxDereferenceDeviceContext ("Reload Temp Use", DeviceContext, DCREF_TEMP_USE);

	*NdisStatus = status;

	IF_LPXDBG (LPX_DEBUG_PNP) {
		LpxPrint2 ("LEAVE LpxReInitializeDeviceContext for %S with Status %08x\n",
						ExportName->Buffer,
						status);
	}

	return;
}

