#pragma once

// 리소스 ID 정의. Code.rc 와 main.cpp 가 공유.
// ICON 리소스 중 *수치가 가장 작은* ID 가 EXE 의 셸 아이콘이 된다 (Windows Explorer 표시용).
#define IDI_APPICON   100

// 앱 버전 — 빌드할 때마다 APP_VER_BUILD 를 1씩 올린다.
#define APP_VER_MAJOR  0
#define APP_VER_MINOR  1
#define APP_VER_PATCH  0
#define APP_VER_BUILD  62
#define APP_VER_STR   "0.1.0.62"
#define APP_VER_WSTR  L"0.1.0.62"
