#pragma once

#include <windows.h>

// O Windows SDK pode tentar redeclarar NTSTATUS ao incluir netioapi.h
// depois de windows.h. Marcamos a definição como já disponível para evitar
// o conflito, mantendo GetIfTable2/MIB_IF_TABLE2 acessíveis.
#ifndef _NTDEF_
#define _NTDEF_
#endif

#include <netioapi.h>
