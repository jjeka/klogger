#include <Ntifs.h>
#include "../LoggerInternal.h"
#include "../Logger.h"

static BOOL GetRegValueDword(HANDLE KeyHandle, PCWSTR ValueName, PULONG pValue, ULONG DefaultValue)
{
	NTSTATUS                       status;
	KEY_VALUE_PARTIAL_INFORMATION  *pInformation;
	ULONG                          uInformationSize;
	UNICODE_STRING	               UnicodeValueName;

	RtlInitUnicodeString(&UnicodeValueName, ValueName);

	uInformationSize = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG);

	pInformation = MemoryAlloc(uInformationSize, PagedPool);
	if (pInformation == NULL)
	{
		ZwClose(KeyHandle);
		return FALSE;
	}
	status = ZwQueryValueKey(KeyHandle, &UnicodeValueName, KeyValuePartialInformation,
		pInformation, uInformationSize, &uInformationSize);

	if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
	{
		MemoryFree(pInformation, uInformationSize);
		ZwClose(KeyHandle);
		return FALSE;
	}
	if (pInformation->Type == REG_DWORD &&
		pInformation->DataLength == sizeof(ULONG))
	{
		RtlCopyMemory(pValue, pInformation->Data, sizeof(ULONG));
	}
	else
	{
		status = ZwSetValueKey(KeyHandle, &UnicodeValueName, 0, REG_DWORD, &DefaultValue, sizeof(ULONG));
		if (!NT_SUCCESS(status))
		{
			ZwClose(KeyHandle);
			MemoryFree(pInformation, uInformationSize);
			return FALSE;
		}
		*pValue = DefaultValue;
		return TRUE;
	}

	ZwClose(KeyHandle);
	MemoryFree(pInformation, uInformationSize);

	return TRUE;
}


#define GET_VALUE(str, defaultValue) \
do { \
	if (!GetRegValueDword(KeyHandle, str, &value, defaultValue)) \
	{\
		ZwClose(KeyHandle); \
		return Parameters; \
	} \
} while (0)

LInitializationParameters LInitializeParameters(char* FileName, PUNICODE_STRING RegPath)
{
	HANDLE KeyHandle;
	OBJECT_ATTRIBUTES ObjectAttributes;
	NTSTATUS status;
	ULONG value;
	LInitializationParameters Parameters;

	Parameters.Status = FALSE;

	InitializeObjectAttributes(&ObjectAttributes, RegPath, 0, NULL, NULL);

	status = ZwOpenKey(&KeyHandle,
		KEY_QUERY_VALUE,
		&ObjectAttributes);

	if (!NT_SUCCESS(status))
		return Parameters;

	strncpy(FileName, "C:\\Users\\Jeka\\Desktop\\Log.txt", MAX_LOG_FILENAME_SIZE);

	GET_VALUE(L"LogLevel", LDBG);
	Logger->Level = value;
	GET_VALUE(L"OutputDbg", FALSE);
	Logger->OutputDbg = (value != 0);
	GET_VALUE(L"NumIdentificators", 10);
	Logger->IdentificatorsSize = value;
	GET_VALUE(L"Timeout", 10 * 1000);
	Logger->Timeout = value;
	GET_VALUE(L"FlushPercent", 90);
	Logger->FlushPercent = value;

	GET_VALUE(L"BufferSize", 1024 * 1024);
	Parameters.RingBufferSize = value;
	GET_VALUE(L"NonPagedPool", TRUE);
	Parameters.NonPagedPool = (value != 0);
	GET_VALUE(L"WaitAtPassive", FALSE);
	Parameters.WaitAtPassive = (value != 0);

	Parameters.Status = TRUE;
	ZwClose(KeyHandle);
	return Parameters;
}

#undef GET_VALUE

KSTART_ROUTINE ThreadFunction;
VOID ThreadFunction(PVOID Param)
{
	NTSTATUS Status, Status2;
	IO_STATUS_BLOCK sb;
	char* ptr;
	size_t size;
	LARGE_INTEGER Timeout;
	PVOID Objects[2];
	UNREFERENCED_PARAMETER(Param);
	Objects[0] = Logger->DoneEvent;
	Objects[1] = Logger->FlushEvent;

	Timeout.QuadPart = -10 * 1000 * Logger->Timeout;

	for (;;)
	{
		Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode, 
			FALSE, (Logger->Timeout == 0xFFFFFFF) ? NULL : &Timeout, NULL);
		if (!NT_SUCCESS(Status))
		{
			DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "KeWaitForMultipleObjects failed with status %d. Contunue", (int)Status);
			continue;
		}

		while ((ptr = RBGetReadPTR(&Logger->RB, &size)) != NULL)
		{
			Status2 = ZwWriteFile(Logger->File, NULL, NULL, NULL, &sb, (PVOID)ptr, (ULONG)size, NULL, NULL);
			if (!NT_SUCCESS(Status2))
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ZwWriteFile failed with status %d", (int)Status);
			if (Logger->OutputDbg)
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "%.*s", (int)size, ptr);
			RBRelease(&Logger->RB, size);
		}
		if (Status == STATUS_WAIT_0 + 0) // DoneEvent
			break;
	}
}

static PVOID CreateEvent(PCWSTR Name)
{
	NTSTATUS Status;
	HANDLE hEvent;
	PVOID pEvent;
	OBJECT_ATTRIBUTES oa;
	UNICODE_STRING us;

	RtlInitUnicodeString(&us, Name);
	InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
	Status = ZwCreateEvent(&hEvent, EVENT_ALL_ACCESS, &oa, NotificationEvent, FALSE);
	if (!NT_SUCCESS(Status))
		return NULL;
	Status = ObReferenceObjectByHandle(hEvent, EVENT_ALL_ACCESS, NULL, KernelMode, &pEvent, NULL);
	if (!NT_SUCCESS(Status))
	{
		ZwClose(hEvent);
		return NULL;
	}
	return pEvent;
}

LErrorCode LInitializeObjects(char* FileName)
{
	NTSTATUS Status;
	OBJECT_ATTRIBUTES oa;
	UNICODE_STRING us;
	IO_STATUS_BLOCK sb;
	UNREFERENCED_PARAMETER(FileName);

	Logger->DoneEvent = CreateEvent(L"\\BaseNamedObjects\\LoggerDoneEvent");
	if (!Logger->DoneEvent)
		return LERROR_CREATE_EVENT;
	Logger->FlushEvent = CreateEvent(L"\\BaseNamedObjects\\LoggerFlushEvent");
	if (!Logger->FlushEvent)
	{
		ObDereferenceObject(Logger->DoneEvent);
		return LERROR_CREATE_EVENT;
	}
	RtlInitUnicodeString(&us, L"C:\\Users\\jeka\\Desktop\\Log.txt");
	InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
	Status = ZwCreateFile(&Logger->File, GENERIC_WRITE, &oa, &sb, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ, FILE_SUPERSEDE, FILE_SEQUENTIAL_ONLY, NULL, 0);
	if (!NT_SUCCESS(Status))
	{
		ObDereferenceObject(Logger->DoneEvent);
		ObDereferenceObject(Logger->FlushEvent);
		return LERROR_OPEN_FILE;
	}
	Status = PsCreateSystemThread(&Logger->Thread, 0, NULL, NULL, NULL, ThreadFunction, NULL);
	if (!NT_SUCCESS(Status))
	{
		ObDereferenceObject(Logger->DoneEvent);
		ObDereferenceObject(Logger->FlushEvent);
		ZwClose(Logger->File);
		return LERROR_CREATE_THREAD;
	}
	return LERROR_SUCCESS;
}

void LDestroyObjects()
{
	// TODO:
}

void LSetFlushEvent()
{
	// TODO: KeInsertQueueDpc
	KeSetEvent(Logger->FlushEvent, 0, FALSE);
}

void LGetTime(unsigned Time[NUM_TIME_PARAMETERS])
{
	// TODO:
	UNREFERENCED_PARAMETER(Time);
}