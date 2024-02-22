/* SPDX-License-Identifier: MIT */

#include <sys/stat.h>
#include <new>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "asm/fstack.hpp"
#include "asm/macro.hpp"
#include "asm/main.hpp"
#include "asm/symbol.hpp"
#include "asm/warning.hpp"
#include "error.hpp"
#include "platform.hpp" // S_ISDIR (stat macro)

#define MAXINCPATHS 128

struct Context {
	struct Context *parent;
	struct FileStackNode *fileInfo;
	struct LexerState *lexerState;
	uint32_t uniqueID;
	struct MacroArgs *macroArgs; // Macro args are *saved* here
	uint32_t nbReptIters;
	int32_t forValue;
	int32_t forStep;
	char *forName;
};

static struct Context *contextStack;
static size_t contextDepth = 0;
size_t maxRecursionDepth;

static unsigned int nbIncPaths = 0;
static char const *includePaths[MAXINCPATHS];

static const char *preIncludeName;

static const char *dumpNodeAndParents(struct FileStackNode const *node)
{
	char const *name;

	if (node->type == NODE_REPT) {
		assert(node->parent); // REPT nodes should always have a parent
		struct FileStackReptNode const *reptInfo = (struct FileStackReptNode const *)node;

		name = dumpNodeAndParents(node->parent);
		fprintf(stderr, "(%" PRIu32 ") -> %s", node->lineNo, name);
		for (uint32_t i = reptInfo->iters->size(); i--; )
			fprintf(stderr, "::REPT~%" PRIu32, (*reptInfo->iters)[i]);
	} else {
		name = ((struct FileStackNamedNode const *)node)->name->c_str();
		if (node->parent) {
			dumpNodeAndParents(node->parent);
			fprintf(stderr, "(%" PRIu32 ") -> %s", node->lineNo, name);
		} else {
			fputs(name, stderr);
		}
	}
	return name;
}

void fstk_Dump(struct FileStackNode const *node, uint32_t lineNo)
{
	dumpNodeAndParents(node);
	fprintf(stderr, "(%" PRIu32 ")", lineNo);
}

void fstk_DumpCurrent(void)
{
	if (!contextStack) {
		fputs("at top level", stderr);
		return;
	}
	fstk_Dump(contextStack->fileInfo, lexer_GetLineNo());
}

struct FileStackNode *fstk_GetFileStack(void)
{
	if (!contextStack)
		return NULL;

	struct FileStackNode *node = contextStack->fileInfo;

	// Mark node and all of its parents as referenced if not already so they don't get freed
	while (node && !node->referenced) {
		node->ID = -1;
		node->referenced = true;
		node = node->parent;
	}
	return contextStack->fileInfo;
}

char const *fstk_GetFileName(void)
{
	// Iterating via the nodes themselves skips nested REPTs
	struct FileStackNode const *node = contextStack->fileInfo;

	while (node->type != NODE_FILE)
		node = node->parent;
	return ((struct FileStackNamedNode const *)node)->name->c_str();
}

void fstk_AddIncludePath(char const *path)
{
	if (path[0] == '\0')
		return;
	if (nbIncPaths >= MAXINCPATHS) {
		error("Too many include directories passed from command line\n");
		return;
	}
	size_t len = strlen(path);
	size_t allocSize = len + (path[len - 1] != '/') + 1;
	char *str = (char *)malloc(allocSize);

	if (!str) {
		// Attempt to continue without that path
		error("Failed to allocate new include path: %s\n", strerror(errno));
		return;
	}
	memcpy(str, path, len);
	char *end = str + len - 1;

	if (*end++ != '/')
		*end++ = '/';
	*end = '\0';
	includePaths[nbIncPaths++] = str;
}

void fstk_SetPreIncludeFile(char const *path)
{
	if (preIncludeName)
		warnx("Overriding pre-included filename %s", preIncludeName);
	preIncludeName = path;
	if (verbose)
		printf("Pre-included filename %s\n", preIncludeName);
}

static void printDep(char const *path)
{
	if (dependfile) {
		fprintf(dependfile, "%s: %s\n", targetFileName, path);
		if (generatePhonyDeps)
			fprintf(dependfile, "%s:\n", path);
	}
}

static bool isPathValid(char const *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) != 0)
		return false;

	// Reject directories
	return !S_ISDIR(statbuf.st_mode);
}

std::string *fstk_FindFile(char const *path)
{
	std::string *fullPath = new(std::nothrow) std::string();

	if (!fullPath) {
		error("Failed to allocate string during include path search: %s\n", strerror(errno));
	} else {
		for (size_t i = 0; i <= nbIncPaths; ++i) {
			*fullPath = i ? includePaths[i - 1] : "";
			*fullPath += path;

			if (isPathValid(fullPath->c_str())) {
				printDep(fullPath->c_str());
				return fullPath;
			}
		}
	}

	errno = ENOENT;
	if (generatedMissingIncludes)
		printDep(path);
	return NULL;
}

bool yywrap(void)
{
	uint32_t ifDepth = lexer_GetIFDepth();

	if (ifDepth != 0)
		fatalerror("Ended block with %" PRIu32 " unterminated IF construct%s\n",
			   ifDepth, ifDepth == 1 ? "" : "s");

	if (contextStack->fileInfo->type == NODE_REPT) {
		// The context is a REPT or FOR block, which may loop
		struct FileStackReptNode *fileInfo = (struct FileStackReptNode *)contextStack->fileInfo;

		// If the node is referenced, we can't edit it; duplicate it
		if (contextStack->fileInfo->referenced) {
			struct FileStackReptNode *copy = (struct FileStackReptNode *)malloc(sizeof(*copy));

			// Copy all info but the referencing
			if (copy) {
				memcpy(copy, fileInfo, sizeof(*copy));
				copy->iters = new(std::nothrow) std::vector<uint32_t>();
			}
			if (!copy || !copy->iters)
				fatalerror("Failed to duplicate REPT file node: %s\n", strerror(errno));
			*copy->iters = *fileInfo->iters; // Copies `fileInfo->iters`
			copy->node.next = NULL;
			copy->node.referenced = false;

			fileInfo = copy;
			contextStack->fileInfo = (struct FileStackNode *)fileInfo;
		}

		// If this is a FOR, update the symbol value
		if (contextStack->forName && fileInfo->iters->front() <= contextStack->nbReptIters) {
			// Avoid arithmetic overflow runtime error
			uint32_t forValue = (uint32_t)contextStack->forValue +
				(uint32_t)contextStack->forStep;
			contextStack->forValue = forValue <= INT32_MAX ? forValue
				: -(int32_t)~forValue - 1;
			struct Symbol *sym = sym_AddVar(contextStack->forName,
				contextStack->forValue);

			// This error message will refer to the current iteration
			if (sym->type != SYM_VAR)
				fatalerror("Failed to update FOR symbol value\n");
		}
		// Advance to the next iteration
		fileInfo->iters->front()++;
		// If this wasn't the last iteration, wrap instead of popping
		if (fileInfo->iters->front() <= contextStack->nbReptIters) {
			lexer_RestartRept(contextStack->fileInfo->lineNo);
			contextStack->uniqueID = macro_UseNewUniqueID();
			return false;
		}
	} else if (!contextStack->parent) {
		return true;
	}

	struct Context *context = contextStack;

	contextStack = contextStack->parent;
	assert(contextDepth != 0); // This is never supposed to underflow
	contextDepth--;

	lexer_DeleteState(context->lexerState);
	// Restore args if a macro (not REPT) saved them
	if (context->fileInfo->type == NODE_MACRO) {
		macro_FreeArgs(macro_GetCurrentArgs());
		macro_UseNewArgs(contextStack->macroArgs);
	}
	// Free the file stack node
	if (!context->fileInfo->referenced) {
		if (context->fileInfo->type == NODE_REPT)
			delete ((struct FileStackReptNode *)context->fileInfo)->iters;
		else
			delete ((struct FileStackNamedNode *)context->fileInfo)->name;
		free(context->fileInfo);
	}
	// Free the FOR symbol name
	free(context->forName);
	// Free the entry and make its parent the current entry
	free(context);

	lexer_SetState(contextStack->lexerState);
	macro_SetUniqueID(contextStack->uniqueID);

	return false;
}

// Make sure not to switch the lexer state before calling this, so the saved line no is correct.
// BE CAREFUL! This modifies the file stack directly, you should have set up the file info first.
// Callers should set contextStack->lexerState after this so it is not NULL.
static void newContext(struct FileStackNode *fileInfo)
{
	++contextDepth;
	fstk_NewRecursionDepth(maxRecursionDepth); // Only checks if the max depth was exceeded

	// Save the current `\@` value, to be restored when this context ends
	contextStack->uniqueID = macro_GetUniqueID();

	struct Context *context = (struct Context *)malloc(sizeof(*context));

	if (!context)
		fatalerror("Failed to allocate memory for new context: %s\n", strerror(errno));
	fileInfo->parent = contextStack->fileInfo;
	fileInfo->lineNo = 0; // Init to a default value, see struct definition for info
	fileInfo->referenced = false;
	fileInfo->lineNo = lexer_GetLineNo();
	context->fileInfo = fileInfo;
	context->forName = NULL;

	// Link new entry to its parent so it's reachable later
	// ERRORS SHOULD NOT OCCUR AFTER THIS!
	context->parent = contextStack;
	contextStack = context;
}

void fstk_RunInclude(char const *path)
{
	std::string *fullPath = fstk_FindFile(path);

	if (!fullPath) {
		if (generatedMissingIncludes) {
			if (verbose)
				printf("Aborting (-MG) on INCLUDE file '%s' (%s)\n",
				       path, strerror(errno));
			failedOnMissingInclude = true;
		} else {
			error("Unable to open included file '%s': %s\n", path, strerror(errno));
		}
		return;
	}

	struct FileStackNamedNode *fileInfo = (struct FileStackNamedNode *)malloc(sizeof(*fileInfo));

	if (!fileInfo) {
		error("Failed to alloc file info for INCLUDE: %s\n", strerror(errno));
		return;
	}
	fileInfo->node.type = NODE_FILE;
	fileInfo->name = fullPath; // `fullPath` is already `new`-allocated, so just point to it

	newContext((struct FileStackNode *)fileInfo);
	contextStack->lexerState = lexer_OpenFile(fileInfo->name->c_str());
	if (!contextStack->lexerState)
		fatalerror("Failed to set up lexer for file include\n");
	lexer_SetStateAtEOL(contextStack->lexerState);
	// We're back at top-level, so most things are reset,
	// but not the unique ID, since INCLUDE may be inside a
	// MACRO or REPT/FOR loop
	contextStack->uniqueID = contextStack->parent->uniqueID;
}

// Similar to `fstk_RunInclude`, but not subject to `-MG`, and
// calling `lexer_SetState` instead of `lexer_SetStateAtEOL`.
static void runPreIncludeFile(void)
{
	if (!preIncludeName)
		return;

	std::string *fullPath = fstk_FindFile(preIncludeName);

	if (!fullPath) {
		error("Unable to open included file '%s': %s\n", preIncludeName, strerror(errno));
		return;
	}

	struct FileStackNamedNode *fileInfo = (struct FileStackNamedNode *)malloc(sizeof(*fileInfo));

	if (!fileInfo) {
		error("Failed to alloc file info for pre-include: %s\n", strerror(errno));
		return;
	}
	fileInfo->node.type = NODE_FILE;
	fileInfo->name = fullPath; // `fullPath` is already `new`-allocated, so just point to it

	newContext((struct FileStackNode *)fileInfo);
	contextStack->lexerState = lexer_OpenFile(fileInfo->name->c_str());
	if (!contextStack->lexerState)
		fatalerror("Failed to set up lexer for file include\n");
	lexer_SetState(contextStack->lexerState);
	// We're back at top-level, so most things are reset
	contextStack->uniqueID = macro_UndefUniqueID();
}

void fstk_RunMacro(char const *macroName, struct MacroArgs *args)
{
	struct Symbol *macro = sym_FindExactSymbol(macroName);

	if (!macro) {
		error("Macro \"%s\" not defined\n", macroName);
		return;
	}
	if (macro->type != SYM_MACRO) {
		error("\"%s\" is not a macro\n", macroName);
		return;
	}
	contextStack->macroArgs = macro_GetCurrentArgs();

	// Compute total length of this node's name: <base name>::<macro>
	size_t reptNameLen = 0;
	struct FileStackNode const *node = macro->src;

	if (node->type == NODE_REPT) {
		struct FileStackReptNode const *reptNode = (struct FileStackReptNode const *)node;

		// 4294967295 = 2^32 - 1, aka UINT32_MAX
		reptNameLen += reptNode->iters->size() * strlen("::REPT~4294967295");
		// Look for next named node
		do {
			node = node->parent;
		} while (node->type == NODE_REPT);
	}

	struct FileStackNamedNode const *baseNode = (struct FileStackNamedNode const *)node;
	struct FileStackNamedNode *fileInfo = (struct FileStackNamedNode *)malloc(sizeof(*fileInfo));

	if (fileInfo)
		fileInfo->name = new(std::nothrow) std::string();
	if (!fileInfo || !fileInfo->name) {
		error("Failed to alloc file info for \"%s\": %s\n", macro->name, strerror(errno));
		return;
	}
	fileInfo->node.type = NODE_MACRO;

	// Print the name...
	fileInfo->name->reserve(baseNode->name->length() + reptNameLen + 2 + strlen(macro->name));
	fileInfo->name->append(*baseNode->name);
	if (node->type == NODE_REPT) {
		struct FileStackReptNode const *reptNode = (struct FileStackReptNode const *)node;

		for (uint32_t i = reptNode->iters->size(); i--; ) {
			char buf[sizeof("::REPT~4294967295")]; // UINT32_MAX

			if (sprintf(buf, "::REPT~%" PRIu32, (*reptNode->iters)[i]) < 0)
				fatalerror("Failed to write macro invocation info: %s\n",
					   strerror(errno));
			fileInfo->name->append(buf);
		}
	}
	fileInfo->name->append("::");
	fileInfo->name->append(macro->name);

	newContext((struct FileStackNode *)fileInfo);
	contextStack->lexerState = lexer_OpenFileView("MACRO", macro->macro.value, macro->macro.size,
						      macro->fileLine);
	if (!contextStack->lexerState)
		fatalerror("Failed to set up lexer for macro invocation\n");
	lexer_SetStateAtEOL(contextStack->lexerState);
	contextStack->uniqueID = macro_UseNewUniqueID();
	macro_UseNewArgs(args);
}

static bool newReptContext(int32_t reptLineNo, char *body, size_t size)
{
	uint32_t reptDepth = contextStack->fileInfo->type == NODE_REPT
				? ((struct FileStackReptNode *)contextStack->fileInfo)->iters->size()
				: 0;
	struct FileStackReptNode *fileInfo = (struct FileStackReptNode *)malloc(sizeof(*fileInfo));

	if (fileInfo)
		fileInfo->iters = new std::vector<uint32_t>();
	if (!fileInfo || !fileInfo->iters) {
		error("Failed to alloc file info for REPT: %s\n", strerror(errno));
		return false;
	}
	fileInfo->node.type = NODE_REPT;
	if (reptDepth) {
		// Copy all parent iter counts
		*fileInfo->iters = *((struct FileStackReptNode *)contextStack->fileInfo)->iters;
	}
	fileInfo->iters->insert(fileInfo->iters->begin(), 1);

	newContext((struct FileStackNode *)fileInfo);
	// Correct our line number, which currently points to the `ENDR` line
	contextStack->fileInfo->lineNo = reptLineNo;

	contextStack->lexerState = lexer_OpenFileView("REPT", body, size, reptLineNo);
	if (!contextStack->lexerState)
		fatalerror("Failed to set up lexer for REPT block\n");
	lexer_SetStateAtEOL(contextStack->lexerState);
	contextStack->uniqueID = macro_UseNewUniqueID();
	return true;
}

void fstk_RunRept(uint32_t count, int32_t reptLineNo, char *body, size_t size)
{
	if (count == 0)
		return;
	if (!newReptContext(reptLineNo, body, size))
		return;

	contextStack->nbReptIters = count;
	contextStack->forName = NULL;
}

void fstk_RunFor(char const *symName, int32_t start, int32_t stop, int32_t step,
		     int32_t reptLineNo, char *body, size_t size)
{
	struct Symbol *sym = sym_AddVar(symName, start);

	if (sym->type != SYM_VAR)
		return;

	uint32_t count = 0;

	if (step > 0 && start < stop)
		count = ((int64_t)stop - start - 1) / step + 1;
	else if (step < 0 && stop < start)
		count = ((int64_t)start - stop - 1) / -(int64_t)step + 1;
	else if (step == 0)
		error("FOR cannot have a step value of 0\n");

	if ((step > 0 && start > stop) || (step < 0 && start < stop))
		warning(WARNING_BACKWARDS_FOR, "FOR goes backwards from %d to %d by %d\n",
			start, stop, step);

	if (count == 0)
		return;
	if (!newReptContext(reptLineNo, body, size))
		return;

	contextStack->nbReptIters = count;
	contextStack->forValue = start;
	contextStack->forStep = step;
	contextStack->forName = strdup(symName);
	if (!contextStack->forName)
		fatalerror("Not enough memory for FOR symbol name: %s\n", strerror(errno));
}

void fstk_StopRept(void)
{
	// Prevent more iterations
	contextStack->nbReptIters = 0;
}

bool fstk_Break(void)
{
	if (contextStack->fileInfo->type != NODE_REPT) {
		error("BREAK can only be used inside a REPT/FOR block\n");
		return false;
	}

	fstk_StopRept();
	return true;
}

void fstk_NewRecursionDepth(size_t newDepth)
{
	if (contextDepth > newDepth)
		fatalerror("Recursion limit (%zu) exceeded\n", newDepth);
	maxRecursionDepth = newDepth;
}

void fstk_Init(char const *mainPath, size_t maxDepth)
{
	struct LexerState *state = lexer_OpenFile(mainPath);

	if (!state)
		fatalerror("Failed to open main file\n");
	lexer_SetState(state);
	char const *fileName = lexer_GetFileName();
	struct Context *context = (struct Context *)malloc(sizeof(*contextStack));
	struct FileStackNamedNode *fileInfo = (struct FileStackNamedNode *)malloc(sizeof(*fileInfo));

	if (!context)
		fatalerror("Failed to allocate memory for main context: %s\n", strerror(errno));
	if (fileInfo)
		fileInfo->name = new(std::nothrow) std::string(fileName);
	if (!fileInfo || !fileInfo->name)
		fatalerror("Failed to allocate memory for main file info: %s\n", strerror(errno));

	context->fileInfo = (struct FileStackNode *)fileInfo;
	// lineNo and nbReptIters are unused on the top-level context
	context->fileInfo->parent = NULL;
	context->fileInfo->lineNo = 0; // This still gets written to the object file, so init it
	context->fileInfo->referenced = false;
	context->fileInfo->type = NODE_FILE;

	context->parent = NULL;
	context->lexerState = state;
	context->uniqueID = macro_UndefUniqueID();
	context->nbReptIters = 0;
	context->forValue = 0;
	context->forStep = 0;
	context->forName = NULL;

	// Now that it's set up properly, register the context
	contextStack = context;

	maxRecursionDepth = maxDepth;

	runPreIncludeFile();
}
