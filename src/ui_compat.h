#pragma once

#include <algorithm>
#include <windows.h>

// Compatibilidade local para a camada de UI Win32.
// Mantém NOMINMAX ativo no projeto e disponibiliza a função padrão usada pela prévia.
using std::min;

// Nome correto no Win32 é SS_ENDELLIPSIS. Este alias mantém o código da UI
// legível sem depender de uma macro inexistente em versões do Windows SDK.
#ifndef SS_END_ELLIPSIS
#define SS_END_ELLIPSIS SS_ENDELLIPSIS
#endif
