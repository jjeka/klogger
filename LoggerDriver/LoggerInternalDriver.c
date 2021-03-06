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

static BOOL GetRegValueString(HANDLE KeyHandle, PCWSTR ValueName, PWSTR pValue, DWORD ValueMaxSize, 
	PCWSTR DefaultValue, DWORD DefaultValueSize)
{
	NTSTATUS                       status;
	KEY_VALUE_PARTIAL_INFORMATION  *pInformation;
	ULONG                          uInformationSize;
	UNICODE_STRING	               UnicodeValueName;

	RtlInitUnicodeString(&UnicodeValueName, ValueName);

	uInformationSize = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + ValueMaxSize * sizeof(WCHAR);

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
	if (pInformation->Type == REG_SZ)
	{
		RtlCopyMemory(pValue, pInformation->Data, pInformation->DataLength);
		pValue[pInformation->DataLength] = 0;
	}
	else
	{
		status = ZwSetValueKey(KeyHandle, &UnicodeValueName, 0, REG_SZ, (PVOID) DefaultValue, DefaultValueSize * sizeof(WCHAR));
		if (!NT_SUCCESS(status))
		{
			ZwClose(KeyHandle);
			MemoryFree(pInformation, uInformationSize);
			return FALSE;
		}
		RtlCopyMemory(pValue, DefaultValue, DefaultValueSize * sizeof(WCHAR));
		pValue[DefaultValueSize] = 0;
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

LInitializationParameters LInitializeParameters(WCHAR* FileName, PUNICODE_STRING RegPath)
{
	HANDLE KeyHandle;
	OBJECT_ATTRIBUTES ObjectAttributes;
	NTSTATUS status;
	ULONG value;
	LInitializationParameters Parameters;
	const WCHAR* DefaultFileName = L"\\??\\C:\\KLog.txt";

	Parameters.Status = FALSE;

	InitializeObjectAttributes(&ObjectAttributes, RegPath, 0, NULL, NULL);

	status = ZwOpenKey(&KeyHandle,
		KEY_QUERY_VALUE,
		&ObjectAttributes);

	if (!NT_SUCCESS(status))
		return Parameters;

	if (!GetRegValueString(KeyHandle, L"LogPath", FileName, MAX_LOG_FILENAME_SIZE, DefaultFileName, 
		(DWORD) wcslen(DefaultFileName)))
	{
		ZwClose(KeyHandle);
		return Parameters;
	}

	GET_VALUE(L"LogLevel", LDBG);
	Logger->Level = value;
	GET_VALUE(L"OutputDbg", FALSE);
	Logger->OutputDbg = (value != 0);
	GET_VALUE(L"NumIdentificators", 10);
	Logger->IdCount = value;
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
	NTSTATUS Status;
	IO_STATUS_BLOCK sb;
	char* ptr;
	size_t size;
	LARGE_INTEGER Timeout;
	PVOID Objects[2];
	UNREFERENCED_PARAMETER(Param);
	Objects[0] = Logger->DoneEvent;
	Objects[1] = Logger->FlushEvent;

	Timeout.QuadPart = -10LL * 1000LL * (ULONGLONG) Logger->Timeout;

	while (Logger)
	{
		Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode, 
			FALSE, (Logger->Timeout == 0xFFFFFFF) ? NULL : &Timeout, NULL);
		if (!NT_SUCCESS(Status))
		{
			DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "KeWaitForMultipleObjects failed with status %X\n", (int)Status);
			break;
		}

		while ((ptr = RBGetReadPTR(&Logger->RB, &size)) != NULL)
		{
			Status = ZwWriteFile(Logger->File, NULL, NULL, NULL, &sb, (PVOID)ptr, (ULONG)size, NULL, NULL);
			if (!NT_SUCCESS(Status))
				DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "ZwWriteFile failed with status %X\n", (int)Status);
			if (Logger->OutputDbg)
				DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%.*s", (int)size, ptr);
			RBRelease(&Logger->RB, size);
		}
		if (Status == STATUS_WAIT_0 + 0) // DoneEvent
			break;
	}
}

static PVOID CreateEvent()
{
	NTSTATUS Status;
	HANDLE hEvent;
	PVOID pEvent;

	Status = ZwCreateEvent(&hEvent, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
	if (!NT_SUCCESS(Status))
	{
		return NULL;
	}
	Status = ObReferenceObjectByHandle(hEvent, EVENT_ALL_ACCESS, NULL, KernelMode, &pEvent, NULL);
	if (!NT_SUCCESS(Status))
	{
		ZwClose(hEvent);
		return NULL;
	}
	return pEvent;
}

KDEFERRED_ROUTINE FlushDeferredRoutine;

VOID FlushDeferredRoutine(struct _KDPC *Dpc, PVOID DeferredContext,
	PVOID SystemArgument1, PVOID SystemArgument2)
{
	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(DeferredContext);
	UNREFERENCED_PARAMETER(SystemArgument1);
	UNREFERENCED_PARAMETER(SystemArgument2);
	KeSetEvent(Logger->FlushEvent, 0, FALSE);
}

LErrorCode LInitializeObjects(WCHAR* FileName)
{
	NTSTATUS Status;
	OBJECT_ATTRIBUTES oa;
	UNICODE_STRING us;
	IO_STATUS_BLOCK sb;
	HANDLE Thread;
	UNREFERENCED_PARAMETER(FileName);

	Logger->DoneEvent = CreateEvent();
	if (!Logger->DoneEvent)
		return LERROR_CREATE_EVENT;
	Logger->FlushEvent = CreateEvent();
	if (!Logger->FlushEvent)
	{
		ObDereferenceObject(Logger->DoneEvent);
		return LERROR_CREATE_EVENT;
	}
	RtlInitUnicodeString(&us, FileName);
	InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
	Status = ZwCreateFile(&Logger->File, FILE_APPEND_DATA, &oa, &sb, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (!NT_SUCCESS(Status))
	{
		ObDereferenceObject(Logger->DoneEvent);
		ObDereferenceObject(Logger->FlushEvent);
		return LERROR_OPEN_FILE;
	}
	Status = PsCreateSystemThread(&Thread, 0, NULL, NULL, NULL, ThreadFunction, NULL);
	if (!NT_SUCCESS(Status))
	{
		ObDereferenceObject(Logger->DoneEvent);
		ObDereferenceObject(Logger->FlushEvent);
		ZwClose(Logger->File);
		return LERROR_CREATE_THREAD;
	}
	Status = ObReferenceObjectByHandle(Thread, EVENT_ALL_ACCESS, NULL, KernelMode, &Logger->Thread, NULL);
	if (!NT_SUCCESS(Status))
	{
		ObDereferenceObject(Logger->DoneEvent);
		ObDereferenceObject(Logger->FlushEvent);
		ZwClose(Logger->File);
		return LERROR_CREATE_THREAD;
	}
	KeInitializeDpc(&Logger->FlushDpc, FlushDeferredRoutine, NULL);
	return LERROR_SUCCESS;
}

void LDestroyObjects()
{
	KeRemoveQueueDpc(&Logger->FlushDpc);
	KeSetEvent(Logger->DoneEvent, 0, FALSE);
	KeWaitForSingleObject(Logger->Thread, Executive, KernelMode, FALSE, NULL);
	ObDereferenceObject(Logger->DoneEvent);
	ObDereferenceObject(Logger->FlushEvent);
	ObDereferenceObject(Logger->Thread);
	ZwClose(Logger->File);
}

void LSetFlushEvent()
{
	KeInsertQueueDpc(&Logger->FlushDpc, NULL, NULL);
}

void LGetTime(unsigned Time[NUM_TIME_PARAMETERS])
{
	LARGE_INTEGER SystemTime;
	LARGE_INTEGER LocalTime;
	TIME_FIELDS TimeFields;
	KeQuerySystemTime(&SystemTime);
	ExSystemTimeToLocalTime(&SystemTime, &LocalTime);
	RtlTimeToTimeFields(&LocalTime, &TimeFields);

	Time[TIME_YEAR] = TimeFields.Year;
	Time[TIME_MONTH] = TimeFields.Month;
	Time[TIME_DAY] = TimeFields.Day;
	Time[TIME_HOUR] = TimeFields.Hour;
	Time[TIME_MINUTE] = TimeFields.Minute;
	Time[TIME_SECOND] = TimeFields.Second;
	Time[TIME_MILLISECONDS] = TimeFields.Milliseconds;
}