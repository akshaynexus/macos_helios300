// SPDX-License-Identifier: GPL-2.0-only
// air-reader-check.c — run the Metal plugin's in-process AIR reader sequence on Linux (tools/release/check-air-llvm.py).
//
//   air-reader-check <libLLVM.so> <unroll pipeline> <module.bc>...
//
// The plugin (driver repo plugin/NVMTLLibraryLoad.m, nvmtl_air_in_memory) resolves the LLVM C API by name from
// air-runtime/libLLVM.dylib and, per Metal library: LLVMContextCreate, a diagnostic handler that fails the module on
// an error-severity diagnostic, LLVMCreateMemoryBufferWithMemoryRangeCopy, LLVMParseBitcodeInContext2, optionally
// LLVMRunPasses (the simdgroup-matrix unroll pipeline; "always-inline,function(sroa,simplifycfg)" and
// "function(instcombine,simplifycfg)" for NVMTL_CANONICAL_POINTERS), LLVMPrintModuleToString. This does the same
// calls, through dlopen/dlsym like the plugin, against an LLVM of the same version built for Linux, on real AIR
// (Apple metal compiler output), so a release proves that this LLVM version reads AIR and runs those pipelines on it.
// The macOS dylib itself cannot run on Linux; check-air-llvm.py checks its exports separately.
// Exit 0 every module parsed, passed and printed with an AIR target triple; 1 otherwise.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *(*ContextCreate)(void);
static void (*ContextDispose)(void *);
static void (*ContextSetDiagnosticHandler)(void *, void (*)(void *, void *), void *);
static void *(*CreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
static void (*DisposeMemoryBuffer)(void *);
static int (*ParseBitcodeInContext2)(void *, void *, void **);
static char *(*PrintModuleToString)(void *);
static void (*DisposeModule)(void *);
static void (*DisposeMessage)(char *);
static char *(*GetDiagInfoDescription)(void *);
static int (*GetDiagInfoSeverity)(void *);
static void *(*RunPasses)(void *, const char *, void *, void *);
static void *(*CreatePassBuilderOptions)(void);
static void (*DisposePassBuilderOptions)(void *);
static char *(*GetErrorMessage)(void *);
static void (*DisposeErrorMessage)(char *);

static int g_error;
static void diag(void *d, void *ctx) {
	(void)ctx;
	if (GetDiagInfoSeverity(d) == 0) g_error = 1;   // LLVMDSError
	char *m = GetDiagInfoDescription(d);
	if (m) { fprintf(stderr, "  reader: %s\n", m); DisposeMessage(m); }
}

static int run(void *module, const char *pipeline) {
	void *opts = CreatePassBuilderOptions();
	void *err = RunPasses(module, pipeline, NULL, opts);
	DisposePassBuilderOptions(opts);
	if (!err) return 0;
	char *m = GetErrorMessage(err);
	fprintf(stderr, "  passes '%s': %s\n", pipeline, m ? m : "?");
	if (m) DisposeErrorMessage(m);
	return 1;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s libLLVM.so pipeline module.bc...\n", argv[0]); return 2; }
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "air-reader-check: dlopen %s: %s\n", argv[1], dlerror()); return 1; }
#define L(n) do { *(void **)&n = dlsym(h, "LLVM" #n); if (!n) { fprintf(stderr, "air-reader-check: LLVM" #n " missing\n"); return 1; } } while (0)
	L(ContextCreate); L(ContextDispose); L(ContextSetDiagnosticHandler); L(CreateMemoryBufferWithMemoryRangeCopy);
	L(DisposeMemoryBuffer); L(ParseBitcodeInContext2); L(PrintModuleToString); L(DisposeModule); L(DisposeMessage);
	L(GetDiagInfoDescription); L(GetDiagInfoSeverity); L(RunPasses); L(CreatePassBuilderOptions);
	L(DisposePassBuilderOptions); L(GetErrorMessage); L(DisposeErrorMessage);
	int bad = 0;
	for (int i = 3; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) { fprintf(stderr, "%s: cannot open\n", argv[i]); bad++; continue; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		char *buf = malloc((size_t)n);
		if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); bad++; continue; }
		fclose(f);
		void *ctx = ContextCreate();
		ContextSetDiagnosticHandler(ctx, diag, NULL);
		void *mb = CreateMemoryBufferWithMemoryRangeCopy(buf, (size_t)n, "nvmtl-air");
		void *mod = NULL;
		g_error = 0;
		int ok = mb && !ParseBitcodeInContext2(ctx, mb, &mod) && mod && !g_error;
		if (ok) ok = !run(mod, argv[2]) && !run(mod, "always-inline,function(sroa,simplifycfg)")
		             && !run(mod, "function(instcombine,simplifycfg)");
		char *text = ok ? PrintModuleToString(mod) : NULL;
		if (text && strstr(text, "target triple = \"air64")) {
			printf("  ok   %s: %ld B of AIR parsed, 3 pipelines run, %zu B of IR printed\n", argv[i], n, strlen(text));
		} else {
			printf("  FAIL %s\n", argv[i]); bad++;
		}
		if (text) DisposeMessage(text);
		if (mod) DisposeModule(mod);
		if (mb) DisposeMemoryBuffer(mb);
		ContextDispose(ctx);
		free(buf);
	}
	printf("air-reader-check: %s (%d of %d modules)\n", bad ? "FAIL" : "PASS", argc - 3 - bad, argc - 3);
	return bad ? 1 : 0;
}
