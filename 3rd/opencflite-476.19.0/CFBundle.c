/*
 * Copyright (c) 2008-2009 Brent Fulgham <bfulgham@gmail.org>.  All rights reserved.
 * Copyright (c) 2009 Grant Erickson <gerickson@nuovations.com>. All rights reserved.
 *
 * This source code is a modified version of the CoreFoundation sources released by Apple Inc. under
 * the terms of the APSL version 2.0 (see below).
 *
 * For information about changes from the original Apple source release can be found by reviewing the
 * source control system for the project at https://sourceforge.net/svn/?group_id=246198.
 *
 * The original license information is as follows:
 * 
 * Copyright (c) 2008 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*	CFBundle.c
	Copyright (c) 1999-2007 Apple Inc.  All rights reserved.
	Responsibility: Doug Davidson
*/

#include "CFBundle_Internal.h"
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFError.h>
#include <string.h>
#include "CFPriv.h"
#include "CFInternal.h"
#include <CoreFoundation/CFByteOrder.h>
#include "CFBundle_BinaryTypes.h"
#include <ctype.h>
#include <sys/stat.h>
#include <stdlib.h>

#if defined(BINARY_SUPPORT_DYLD)
// Import the mach-o headers that define the macho magic numbers
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/arch.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif /* BINARY_SUPPORT_DYLD */

#if defined(BINARY_SUPPORT_DLFCN)
#include <dlfcn.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_SOLARIS
#define CF_RTLD_FIRST	RTLD_FIRST
#else
#define CF_RTLD_FIRST	0
#endif /* DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_SOLARIS */
#endif /* BINARY_SUPPORT_DLFCN */

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_WINDOWS || DEPLOYMENT_TARGET_LINUX
#include <fcntl.h>
#endif

#if DEPLOYMENT_TARGET_WINDOWS
#include <io.h>
#include <stdio.h>
#define lseek _lseek
#define open _open
#define read _read
#define write _write
#define close _close
#endif

#if DEPLOYMENT_TARGET_LINUX
#include <unistd.h>
#endif

#define LOG_BUNDLE_LOAD 0

// Public CFBundle Info plist keys
CONST_STRING_DECL(kCFBundleInfoDictionaryVersionKey, "CFBundleInfoDictionaryVersion")
CONST_STRING_DECL(kCFBundleExecutableKey, "CFBundleExecutable")
CONST_STRING_DECL(kCFBundleIdentifierKey, "CFBundleIdentifier")
CONST_STRING_DECL(kCFBundleVersionKey, "CFBundleVersion")
CONST_STRING_DECL(kCFBundleDevelopmentRegionKey, "CFBundleDevelopmentRegion")
CONST_STRING_DECL(kCFBundleLocalizationsKey, "CFBundleLocalizations")

// Finder stuff
CONST_STRING_DECL(_kCFBundlePackageTypeKey, "CFBundlePackageType")
CONST_STRING_DECL(_kCFBundleSignatureKey, "CFBundleSignature")
CONST_STRING_DECL(_kCFBundleIconFileKey, "CFBundleIconFile")
CONST_STRING_DECL(_kCFBundleDocumentTypesKey, "CFBundleDocumentTypes")
CONST_STRING_DECL(_kCFBundleURLTypesKey, "CFBundleURLTypes")

// Keys that are usually localized in InfoPlist.strings
CONST_STRING_DECL(kCFBundleNameKey, "CFBundleName")
CONST_STRING_DECL(_kCFBundleDisplayNameKey, "CFBundleDisplayName")
CONST_STRING_DECL(_kCFBundleShortVersionStringKey, "CFBundleShortVersionString")
CONST_STRING_DECL(_kCFBundleGetInfoStringKey, "CFBundleGetInfoString")
CONST_STRING_DECL(_kCFBundleGetInfoHTMLKey, "CFBundleGetInfoHTML")

// Sub-keys for CFBundleDocumentTypes dictionaries
CONST_STRING_DECL(_kCFBundleTypeNameKey, "CFBundleTypeName")
CONST_STRING_DECL(_kCFBundleTypeRoleKey, "CFBundleTypeRole")
CONST_STRING_DECL(_kCFBundleTypeIconFileKey, "CFBundleTypeIconFile")
CONST_STRING_DECL(_kCFBundleTypeOSTypesKey, "CFBundleTypeOSTypes")
CONST_STRING_DECL(_kCFBundleTypeExtensionsKey, "CFBundleTypeExtensions")
CONST_STRING_DECL(_kCFBundleTypeMIMETypesKey, "CFBundleTypeMIMETypes")

// Sub-keys for CFBundleURLTypes dictionaries
CONST_STRING_DECL(_kCFBundleURLNameKey, "CFBundleURLName")
CONST_STRING_DECL(_kCFBundleURLIconFileKey, "CFBundleURLIconFile")
CONST_STRING_DECL(_kCFBundleURLSchemesKey, "CFBundleURLSchemes")

// Compatibility key names
CONST_STRING_DECL(_kCFBundleOldExecutableKey, "NSExecutable")
CONST_STRING_DECL(_kCFBundleOldInfoDictionaryVersionKey, "NSInfoPlistVersion")
CONST_STRING_DECL(_kCFBundleOldNameKey, "NSHumanReadableName")
CONST_STRING_DECL(_kCFBundleOldIconFileKey, "NSIcon")
CONST_STRING_DECL(_kCFBundleOldDocumentTypesKey, "NSTypes")
CONST_STRING_DECL(_kCFBundleOldShortVersionStringKey, "NSAppVersion")

// Compatibility CFBundleDocumentTypes key names
CONST_STRING_DECL(_kCFBundleOldTypeNameKey, "NSName")
CONST_STRING_DECL(_kCFBundleOldTypeRoleKey, "NSRole")
CONST_STRING_DECL(_kCFBundleOldTypeIconFileKey, "NSIcon")
CONST_STRING_DECL(_kCFBundleOldTypeExtensions1Key, "NSUnixExtensions")
CONST_STRING_DECL(_kCFBundleOldTypeExtensions2Key, "NSDOSExtensions")
CONST_STRING_DECL(_kCFBundleOldTypeOSTypesKey, "NSMacOSType")

// Internally used keys for loaded Info plists.
CONST_STRING_DECL(_kCFBundleInfoPlistURLKey, "CFBundleInfoPlistURL")
CONST_STRING_DECL(_kCFBundleRawInfoPlistURLKey, "CFBundleRawInfoPlistURL")
CONST_STRING_DECL(_kCFBundleNumericVersionKey, "CFBundleNumericVersion")
CONST_STRING_DECL(_kCFBundleExecutablePathKey, "CFBundleExecutablePath")
CONST_STRING_DECL(_kCFBundleResourcesFileMappedKey, "CSResourcesFileMapped")
CONST_STRING_DECL(_kCFBundleCFMLoadAsBundleKey, "CFBundleCFMLoadAsBundle")
CONST_STRING_DECL(_kCFBundleAllowMixedLocalizationsKey, "CFBundleAllowMixedLocalizations")

// Keys used by NSBundle for loaded Info plists.
CONST_STRING_DECL(_kCFBundleInitialPathKey, "NSBundleInitialPath")
CONST_STRING_DECL(_kCFBundleResolvedPathKey, "NSBundleResolvedPath")
CONST_STRING_DECL(_kCFBundlePrincipalClassKey, "NSPrincipalClass")

static CFTypeID __kCFBundleTypeID = _kCFRuntimeNotATypeID;

struct __CFBundle {
    CFRuntimeBase _base;

    CFURLRef _url;
    CFDateRef _modDate;

    CFDictionaryRef _infoDict;
    CFDictionaryRef _localInfoDict;
    CFArrayRef _searchLanguages;

    __CFPBinaryType _binaryType;
    Boolean _isLoaded;
    uint8_t _version;
    Boolean _sharesStringsFiles;
    char _padding[1];

    /* CFM goop */
    void *_connectionCookie;

    /* DYLD goop */
    const void *_imageCookie;
    const void *_moduleCookie;

    /* dlfcn goop */
    void *_handleCookie;
    
    /* CFM<->DYLD glue */
    CFMutableDictionaryRef _glueDict;
    
    /* Resource fork goop */
    _CFResourceData _resourceData;

    _CFPlugInData _plugInData;

#if defined(BINARY_SUPPORT_DLL)
    HMODULE _hModule;
#endif /* BINARY_SUPPORT_DLL */

};

static CFSpinLock_t CFBundleGlobalDataLock = CFSpinLockInit;

static CFMutableDictionaryRef _bundlesByURL = NULL;
static CFMutableDictionaryRef _bundlesByIdentifier = NULL;

// For scheduled lazy unloading.  Used by CFPlugIn.
static CFMutableSetRef _bundlesToUnload = NULL;
static Boolean _scheduledBundlesAreUnloading = false;

// Various lists of all bundles.
static CFMutableArrayRef _allBundles = NULL;

static Boolean _initedMainBundle = false;
static CFBundleRef _mainBundle = NULL;
static CFStringRef _defaultLocalization = NULL;

static Boolean _useDlfcn = false;

// Forward declares functions.
static CFBundleRef _CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL, Boolean alreadyLocked, Boolean doFinalProcessing);
static CFStringRef _CFBundleCopyExecutableName(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFDictionaryRef infoDict);
static CFURLRef _CFBundleCopyExecutableURLIgnoringCache(CFBundleRef bundle);
static void _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(CFStringRef hint);
static void _CFBundleEnsureAllBundlesUpToDateAlreadyLocked(void);
static void _CFBundleCheckWorkarounds(CFBundleRef bundle);
static void _CFBundleEnsureBundleExistsForImagePath(CFStringRef imagePath);
static void _CFBundleEnsureBundlesExistForImagePaths(CFArrayRef imagePaths);
#if defined(BINARY_SUPPORT_DYLD)
static CFDictionaryRef _CFBundleGrokInfoDictFromMainExecutable(void);
static Boolean _CFBundleGrokObjCImageInfoFromMainExecutable(uint32_t *objcVersion, uint32_t *objcFlags);
static CFStringRef _CFBundleDYLDCopyLoadedImagePathForPointer(void *p);
static void *_CFBundleDYLDGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch);
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLFCN)
static CFStringRef _CFBundleDlfcnCopyLoadedImagePathForPointer(void *p);
static void *_CFBundleDlfcnGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch);
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM)
static void *_CFBundleFunctionPointerForTVector(CFAllocatorRef allocator, void *tvp);
static void *_CFBundleTVectorForFunctionPointer(CFAllocatorRef allocator, void *fp);
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM */

static void _CFBundleAddToTables(CFBundleRef bundle, Boolean alreadyLocked) {
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);

    if (!alreadyLocked) __CFSpinLock(&CFBundleGlobalDataLock);
    
    // Add to the _allBundles list
    if (!_allBundles) {
        // Create this from the default allocator
        CFArrayCallBacks nonRetainingArrayCallbacks = kCFTypeArrayCallBacks;
        nonRetainingArrayCallbacks.retain = NULL;
        nonRetainingArrayCallbacks.release = NULL;
        _allBundles = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &nonRetainingArrayCallbacks);
    }
    CFArrayAppendValue(_allBundles, bundle);
    
    // Add to the table that maps urls to bundles
    if (!_bundlesByURL) {
        // Create this from the default allocator
        CFDictionaryValueCallBacks nonRetainingDictionaryValueCallbacks = kCFTypeDictionaryValueCallBacks;
        nonRetainingDictionaryValueCallbacks.retain = NULL;
        nonRetainingDictionaryValueCallbacks.release = NULL;
        _bundlesByURL = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &nonRetainingDictionaryValueCallbacks);
    }
    CFDictionarySetValue(_bundlesByURL, bundle->_url, bundle);

    // Add to the table that maps identifiers to bundles
    if (bundleID) {
        CFMutableArrayRef bundlesWithThisID = NULL;
        CFBundleRef existingBundle = NULL;
        if (!_bundlesByIdentifier) {
            // Create this from the default allocator
            _bundlesByIdentifier = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        bundlesWithThisID = (CFMutableArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
        if (bundlesWithThisID) {
            CFIndex i, count = CFArrayGetCount(bundlesWithThisID);
            UInt32 existingVersion, newVersion = CFBundleGetVersionNumber(bundle);
            for (i = 0; i < count; i++) {
                existingBundle = (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, i);
                existingVersion = CFBundleGetVersionNumber(existingBundle);
                // If you load two bundles with the same identifier and the same version, the last one wins.
                if (newVersion >= existingVersion) break;
            }
            CFArrayInsertValueAtIndex(bundlesWithThisID, i, bundle);
        } else {
            // Create this from the default allocator
            CFArrayCallBacks nonRetainingArrayCallbacks = kCFTypeArrayCallBacks;
            nonRetainingArrayCallbacks.retain = NULL;
            nonRetainingArrayCallbacks.release = NULL;
            bundlesWithThisID = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &nonRetainingArrayCallbacks);
            CFArrayAppendValue(bundlesWithThisID, bundle);
            CFDictionarySetValue(_bundlesByIdentifier, bundleID, bundlesWithThisID);
            CFRelease(bundlesWithThisID);
        }
    }
    if (!alreadyLocked) __CFSpinUnlock(&CFBundleGlobalDataLock);
}

static void _CFBundleRemoveFromTables(CFBundleRef bundle) {
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);

    __CFSpinLock(&CFBundleGlobalDataLock);

    // Remove from the various lists
    if (_allBundles) {
        CFIndex i = CFArrayGetFirstIndexOfValue(_allBundles, CFRangeMake(0, CFArrayGetCount(_allBundles)), bundle);
        if (i >= 0) CFArrayRemoveValueAtIndex(_allBundles, i);
    }

    // Remove from the table that maps urls to bundles
    if (_bundlesByURL) CFDictionaryRemoveValue(_bundlesByURL, bundle->_url);
    
    // Remove from the table that maps identifiers to bundles
    if (bundleID && _bundlesByIdentifier) {
        CFMutableArrayRef bundlesWithThisID = (CFMutableArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
        if (bundlesWithThisID) {
            CFIndex count = CFArrayGetCount(bundlesWithThisID);
            while (count-- > 0) if (bundle == (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, count)) CFArrayRemoveValueAtIndex(bundlesWithThisID, count);
            if (0 == CFArrayGetCount(bundlesWithThisID)) CFDictionaryRemoveValue(_bundlesByIdentifier, bundleID);
        }
    }
    
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ CFBundleRef _CFBundleFindByURL(CFURLRef url, Boolean alreadyLocked) {
    CFBundleRef result = NULL;
    if (!alreadyLocked) __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesByURL) result = (CFBundleRef)CFDictionaryGetValue(_bundlesByURL, url);
    if (!alreadyLocked) __CFSpinUnlock(&CFBundleGlobalDataLock);
    return result;
}

static CFURLRef _CFBundleCopyBundleURLForExecutablePath(CFStringRef str) {
    //!!! need to handle frameworks, NT; need to integrate with NSBundle - drd
    UniChar buff[CFMaxPathSize];
    CFIndex buffLen;
    CFURLRef url = NULL;
    CFStringRef outstr;
    
    buffLen = CFStringGetLength(str);
    if (buffLen > CFMaxPathSize) buffLen = CFMaxPathSize;
    CFStringGetCharacters(str, CFRangeMake(0, buffLen), buff);

    if (!url) {
        buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);  // Remove exe name

        if (buffLen > 0) {
            // See if this is a new bundle.  If it is, we have to remove more path components.
            CFIndex startOfLastDir = _CFStartOfLastPathComponent(buff, buffLen);
            if ((startOfLastDir > 0) && (startOfLastDir < buffLen)) {
                CFStringRef lastDirName = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, &(buff[startOfLastDir]), buffLen - startOfLastDir);

                if (CFEqual(lastDirName, _CFBundleGetPlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetAlternatePlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetOtherPlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName())) {
                    // This is a new bundle.  Back off a few more levels
                    if (buffLen > 0) {
                        // Remove platform folder
                        buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                    }
                    if (buffLen > 0) {
                        // Remove executables folder (if present)
                        CFIndex startOfNextDir = _CFStartOfLastPathComponent(buff, buffLen);
                        if ((startOfNextDir > 0) && (startOfNextDir < buffLen)) {
                            CFStringRef nextDirName = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, &(buff[startOfNextDir]), buffLen - startOfNextDir);
                            if (CFEqual(nextDirName, _CFBundleExecutablesDirectoryName)) buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                            CFRelease(nextDirName);
                        }
                    }
                    if (buffLen > 0) {
                        // Remove support files folder
                        buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                    }
                }
                CFRelease(lastDirName);
            }
        }

        if (buffLen > 0) {
            outstr = CFStringCreateWithCharactersNoCopy(kCFAllocatorSystemDefault, buff, buffLen, kCFAllocatorNull);
            url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, outstr, PLATFORM_PATH_STYLE, true);
            CFRelease(outstr);
        }
    }
    return url;
}

static CFURLRef _CFBundleCopyResolvedURLForExecutableURL(CFURLRef url) {
    // this is necessary so that we match any sanitization CFURL may perform on the result of _CFBundleCopyBundleURLForExecutableURL()
    CFURLRef absoluteURL, url1, url2, outURL = NULL;
    CFStringRef str, str1, str2;
    absoluteURL = CFURLCopyAbsoluteURL(url);
    str = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    if (str) {
        UniChar buff[CFMaxPathSize];
        CFIndex buffLen = CFStringGetLength(str), len1;
        if (buffLen > CFMaxPathSize) buffLen = CFMaxPathSize;
        CFStringGetCharacters(str, CFRangeMake(0, buffLen), buff);
        len1 = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
        if (len1 > 0 && len1 + 1 < buffLen) {
            str1 = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, buff, len1);
            str2 = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, buff + len1 + 1, buffLen - len1 - 1);
            if (str1 && str2) {
                url1 = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str1, PLATFORM_PATH_STYLE, true);
                if (url1) {
                    url2 = CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, str2, PLATFORM_PATH_STYLE, false, url1);
                    if (url2) {
                        outURL = CFURLCopyAbsoluteURL(url2);
                        CFRelease(url2);
                    }
                    CFRelease(url1);
                }
            }
            if (str1) CFRelease(str1);
            if (str2) CFRelease(str2);
        }
        CFRelease(str);
    }
    if (!outURL) {
        outURL = absoluteURL;
    } else {
        CFRelease(absoluteURL);
    }
    return outURL;
}

CFURLRef _CFBundleCopyBundleURLForExecutableURL(CFURLRef url) {
    CFURLRef resolvedURL, outurl = NULL;
    CFStringRef str;
    resolvedURL = _CFBundleCopyResolvedURLForExecutableURL(url);
    str = CFURLCopyFileSystemPath(resolvedURL, PLATFORM_PATH_STYLE);
    if (str) {
        outurl = _CFBundleCopyBundleURLForExecutablePath(str);
        CFRelease(str);
    }
    CFRelease(resolvedURL);
    return outurl;
}

CFBundleRef _CFBundleCreateIfLooksLikeBundle(CFAllocatorRef allocator, CFURLRef url) {
    CFBundleRef bundle = CFBundleCreate(allocator, url);
    
    // exclude type 0 bundles with no binary (or CFM binary) and no Info.plist, since they give too many false positives
    if (bundle && 0 == bundle->_version) {
        CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
        if (!infoDict || 0 == CFDictionaryGetCount(infoDict)) {
#if defined(BINARY_SUPPORT_CFM) && defined(BINARY_SUPPORT_DYLD)
            CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
            if (executableURL) {
                if (bundle->_binaryType == __CFBundleUnknownBinary) bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
                if (bundle->_binaryType == __CFBundleCFMBinary || bundle->_binaryType == __CFBundleUnreadableBinary) {
                    bundle->_version = 4;
                } else {
                    bundle->_resourceData._executableLacksResourceFork = true;
                }
                CFRelease(executableURL);
            } else {
                bundle->_version = 4;
            }
#elif defined(BINARY_SUPPORT_CFM)
            bundle->_version = 4;
#else 
            CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
            if (executableURL) {
                CFRelease(executableURL);
            } else {
                bundle->_version = 4;
            }
#endif /* BINARY_SUPPORT_CFM && BINARY_SUPPORT_DYLD */
        }
    }
    if (bundle && (3 == bundle->_version || 4 == bundle->_version)) {
        CFRelease(bundle);
        bundle = NULL;
    }
    return bundle;
}

CFBundleRef _CFBundleGetMainBundleIfLooksLikeBundle(void) {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle && (3 == mainBundle->_version || 4 == mainBundle->_version)) mainBundle = NULL;
    return mainBundle;
}

Boolean _CFBundleMainBundleInfoDictionaryComesFromResourceFork(void) {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    return (mainBundle && mainBundle->_resourceData._infoDictionaryFromResourceFork);
}

CFBundleRef _CFBundleCreateWithExecutableURLIfLooksLikeBundle(CFAllocatorRef allocator, CFURLRef url) {
    CFBundleRef bundle = NULL;
    CFURLRef bundleURL = _CFBundleCopyBundleURLForExecutableURL(url), resolvedURL = _CFBundleCopyResolvedURLForExecutableURL(url);
    if (bundleURL && resolvedURL) {
        bundle = _CFBundleCreateIfLooksLikeBundle(allocator, bundleURL);
        if (bundle) {
            CFURLRef executableURL = _CFBundleCopyExecutableURLIgnoringCache(bundle);
            char buff1[CFMaxPathSize], buff2[CFMaxPathSize];
            if (!executableURL || !CFURLGetFileSystemRepresentation(resolvedURL, true, (uint8_t *)buff1, CFMaxPathSize) || !CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff2, CFMaxPathSize) || 0 != strcmp(buff1, buff2)) {
                CFRelease(bundle);
                bundle = NULL;
            }
            if (executableURL) CFRelease(executableURL);
        }
    }
    if (bundleURL) CFRelease(bundleURL);
    if (resolvedURL) CFRelease(resolvedURL);
    return bundle;
}

CFURLRef _CFBundleCopyMainBundleExecutableURL(Boolean *looksLikeBundle) {
    // This function is for internal use only; _mainBundle is deliberately accessed outside of the lock to get around a reentrancy issue
    const char *processPath;
    CFStringRef str = NULL;
    CFURLRef executableURL = NULL;
    processPath = _CFProcessPath();
    if (processPath) {
        str = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, processPath);
        if (str) {
            executableURL = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, PLATFORM_PATH_STYLE, false);
            CFRelease(str);
        }
    }
    if (looksLikeBundle) {
        CFBundleRef mainBundle = _mainBundle;
        if (mainBundle && (3 == mainBundle->_version || 4 == mainBundle->_version)) mainBundle = NULL;
        *looksLikeBundle = (mainBundle ? true : false);
    }
    return executableURL;
}

static void _CFBundleInitializeMainBundleInfoDictionaryAlreadyLocked(CFStringRef executablePath) {
#if defined(BINARY_SUPPORT_CFM)
    Boolean versRegionOverrides = false;
#endif /* BINARY_SUPPORT_CFM */
    CFBundleGetInfoDictionary(_mainBundle);
    if (!_mainBundle->_infoDict || CFDictionaryGetCount(_mainBundle->_infoDict) == 0) {
        // if type 3 bundle and no Info.plist, treat as unbundled, since this gives too many false positives
        if (_mainBundle->_version == 3) _mainBundle->_version = 4;
        if (_mainBundle->_version == 0) {
            // if type 0 bundle and no Info.plist and not main executable for bundle, treat as unbundled, since this gives too many false positives
            CFStringRef executableName = _CFBundleCopyExecutableName(kCFAllocatorSystemDefault, _mainBundle, NULL, NULL);
            if (!executableName || !executablePath || !CFStringHasSuffix(executablePath, executableName)) _mainBundle->_version = 4;
            if (executableName) CFRelease(executableName);
        }
#if defined(BINARY_SUPPORT_DYLD)
        if (_mainBundle->_binaryType == __CFBundleDYLDExecutableBinary) {
            if (_mainBundle->_infoDict) CFRelease(_mainBundle->_infoDict);
            _mainBundle->_infoDict = _CFBundleGrokInfoDictFromMainExecutable();
        }
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_CFM)
        if (_mainBundle->_binaryType == __CFBundleCFMBinary || _mainBundle->_binaryType == __CFBundleUnreadableBinary) {
            // if type 0 bundle and CFM binary and no Info.plist, treat as unbundled, since this also gives too many false positives
            if (_mainBundle->_version == 0) _mainBundle->_version = 4;
            if (_mainBundle->_version != 4) {
                // if CFM binary and no Info.plist and not main executable for bundle, treat as unbundled, since this also gives too many false positives
                // except for Macromedia Director MX, which is unbundled but wants to be treated as bundled
                CFStringRef executableName = _CFBundleCopyExecutableName(kCFAllocatorSystemDefault, _mainBundle, NULL, NULL);
                Boolean treatAsBundled = false;
                if (executablePath) {
                    CFIndex strLength = CFStringGetLength(executablePath);
                    if (strLength > 10) treatAsBundled = CFStringFindWithOptions(executablePath, CFSTR(" MX"), CFRangeMake(strLength - 10, 10), 0, NULL);
                }
                if (!treatAsBundled && (!executableName || !executablePath || !CFStringHasSuffix(executablePath, executableName))) _mainBundle->_version = 4;
                if (executableName) CFRelease(executableName);
            }
            if (_mainBundle->_infoDict) CFRelease(_mainBundle->_infoDict);
            if (executablePath) {
                CFURLRef executableURL = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, executablePath, PLATFORM_PATH_STYLE, false);
                if (executableURL) {
                    _mainBundle->_infoDict = _CFBundleCopyInfoDictionaryInResourceForkWithAllocator(CFGetAllocator(_mainBundle), executableURL);
                    if (_mainBundle->_infoDict) _mainBundle->_resourceData._infoDictionaryFromResourceFork = true;
                    CFRelease(executableURL);
                }
            }
            if (_mainBundle->_binaryType == __CFBundleUnreadableBinary && _mainBundle->_infoDict && CFDictionaryGetValue(_mainBundle->_infoDict, kCFBundleDevelopmentRegionKey)) versRegionOverrides = true;
        }
#endif /* BINARY_SUPPORT_CFM */
    }
    if (!_mainBundle->_infoDict) _mainBundle->_infoDict = CFDictionaryCreateMutable(CFGetAllocator(_mainBundle), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!CFDictionaryGetValue(_mainBundle->_infoDict, _kCFBundleExecutablePathKey)) CFDictionarySetValue((CFMutableDictionaryRef)(_mainBundle->_infoDict), _kCFBundleExecutablePathKey, executablePath);
#if defined(BINARY_SUPPORT_CFM)
    if (versRegionOverrides) {
        // This is a hack to preserve backward compatibility for certain broken applications (2761067)
        CFStringRef devLang = _CFBundleCopyBundleDevelopmentRegionFromVersResource(_mainBundle);
        if (devLang) {
            CFDictionarySetValue((CFMutableDictionaryRef)(_mainBundle->_infoDict), kCFBundleDevelopmentRegionKey, devLang);
            CFRelease(devLang);
        }
    }
#endif /* BINARY_SUPPORT_CFM */
}

CF_EXPORT void _CFBundleFlushBundleCaches(CFBundleRef bundle) {
    CFDictionaryRef oldInfoDict = bundle->_infoDict;
    CFTypeRef val;
    
    _CFBundleFlushCachesForURL(bundle->_url);
    bundle->_infoDict = NULL;
    if (bundle->_localInfoDict) {
        CFRelease(bundle->_localInfoDict);
        bundle->_localInfoDict = NULL;
    }
    if (bundle->_searchLanguages) {
        CFRelease(bundle->_searchLanguages);
        bundle->_searchLanguages = NULL;
    }
    if (bundle->_resourceData._stringTableCache) {
        CFRelease(bundle->_resourceData._stringTableCache);
        bundle->_resourceData._stringTableCache = NULL;
    }
    if (bundle == _mainBundle) {
        CFStringRef executablePath = oldInfoDict ? (CFStringRef)CFDictionaryGetValue(oldInfoDict, _kCFBundleExecutablePathKey) : NULL;
        __CFSpinLock(&CFBundleGlobalDataLock);
        _CFBundleInitializeMainBundleInfoDictionaryAlreadyLocked(executablePath);
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    } else {
        CFBundleGetInfoDictionary(bundle);
    }
    if (oldInfoDict) {
        if (!bundle->_infoDict) bundle->_infoDict = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        val = CFDictionaryGetValue(oldInfoDict, _kCFBundleInitialPathKey);
        if (val) CFDictionarySetValue((CFMutableDictionaryRef)bundle->_infoDict, _kCFBundleInitialPathKey, val);
        val = CFDictionaryGetValue(oldInfoDict, _kCFBundleResolvedPathKey);
        if (val) CFDictionarySetValue((CFMutableDictionaryRef)bundle->_infoDict, _kCFBundleResolvedPathKey, val);
        val = CFDictionaryGetValue(oldInfoDict, _kCFBundlePrincipalClassKey);
        if (val) CFDictionarySetValue((CFMutableDictionaryRef)bundle->_infoDict, _kCFBundlePrincipalClassKey, val);
        CFRelease(oldInfoDict);
    }
}

static CFBundleRef _CFBundleGetMainBundleAlreadyLocked(void) {
    if (!_initedMainBundle) {
        const char *processPath;
        CFStringRef str = NULL;
        CFURLRef executableURL = NULL, bundleURL = NULL;
        _initedMainBundle = true;
        processPath = _CFProcessPath();
        if (processPath) {
            str = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, processPath);
            if (!executableURL) executableURL = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, PLATFORM_PATH_STYLE, false);
        }
        if (executableURL) bundleURL = _CFBundleCopyBundleURLForExecutableURL(executableURL);
        if (bundleURL) {
            // make sure that main bundle has executable path
            //??? what if we are not the main executable in the bundle?
            // NB doFinalProcessing must be false here, see below
            _mainBundle = _CFBundleCreate(kCFAllocatorSystemDefault, bundleURL, true, false);
            if (_mainBundle) {
                // make sure that the main bundle is listed as loaded, and mark it as executable
                _mainBundle->_isLoaded = true;
#if defined(BINARY_SUPPORT_DYLD)
                if (_mainBundle->_binaryType == __CFBundleUnknownBinary) {
                    if (!executableURL) {
                        _mainBundle->_binaryType = __CFBundleNoBinary;
                    } else {
                        _mainBundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
                        if (_mainBundle->_binaryType != __CFBundleCFMBinary && _mainBundle->_binaryType != __CFBundleUnreadableBinary) _mainBundle->_resourceData._executableLacksResourceFork = true;
#endif /* BINARY_SUPPORT_CFM */
                    }
                }                
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DYLD)
                // get cookie for already-loaded main bundle
                if (_mainBundle->_binaryType == __CFBundleDYLDExecutableBinary && !_mainBundle->_imageCookie) {
                    // ??? need better way to specify main executable image
                    _mainBundle->_imageCookie = (void *)_dyld_get_image_header(0);
#if LOG_BUNDLE_LOAD
                    printf("main bundle %p getting image %p\n", _mainBundle, _mainBundle->_imageCookie);
#endif /* LOG_BUNDLE_LOAD */
                }
#endif /* BINARY_SUPPORT_DYLD */
                _CFBundleInitializeMainBundleInfoDictionaryAlreadyLocked(str);
                // Perform delayed final processing steps.
                // This must be done after _isLoaded has been set, for security reasons (3624341).
                _CFBundleCheckWorkarounds(_mainBundle);
                if (_CFBundleNeedsInitPlugIn(_mainBundle)) {
                    __CFSpinUnlock(&CFBundleGlobalDataLock);
                    _CFBundleInitPlugIn(_mainBundle);
                    __CFSpinLock(&CFBundleGlobalDataLock);
                }
            }
        }
        if (bundleURL) CFRelease(bundleURL);
        if (str) CFRelease(str);
        if (executableURL) CFRelease(executableURL);
    }
    return _mainBundle;
}

CFBundleRef CFBundleGetMainBundle(void) {
    CFBundleRef mainBundle;
    __CFSpinLock(&CFBundleGlobalDataLock);
    mainBundle = _CFBundleGetMainBundleAlreadyLocked();
    __CFSpinUnlock(&CFBundleGlobalDataLock);
    return mainBundle;
}

CFBundleRef CFBundleGetBundleWithIdentifier(CFStringRef bundleID) {
    CFBundleRef result = NULL;
    CFArrayRef bundlesWithThisID;
    if (bundleID) {
        __CFSpinLock(&CFBundleGlobalDataLock);
        (void)_CFBundleGetMainBundleAlreadyLocked();
        if (_bundlesByIdentifier) {
            bundlesWithThisID = (CFArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
            if (bundlesWithThisID && CFArrayGetCount(bundlesWithThisID) > 0) result = (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, 0);
        }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX
        if (!result) {
            // Try to create the bundle for the caller and try again
            void *p = __builtin_return_address(0);
            if (p) {
                CFStringRef imagePath = NULL;
#if defined(BINARY_SUPPORT_DLFCN)
                if (!imagePath && _useDlfcn) imagePath = _CFBundleDlfcnCopyLoadedImagePathForPointer(p);
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD)
                if (!imagePath) imagePath = _CFBundleDYLDCopyLoadedImagePathForPointer(p);
#endif /* BINARY_SUPPORT_DYLD */
                if (imagePath) {
                    _CFBundleEnsureBundleExistsForImagePath(imagePath);
                    CFRelease(imagePath);
                }
                if (_bundlesByIdentifier) {
                    bundlesWithThisID = (CFArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
                    if (bundlesWithThisID && CFArrayGetCount(bundlesWithThisID) > 0) result = (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, 0);
                }
            }
        }
#endif
        if (!result) {
            // Try to guess the bundle from the identifier and try again
            _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(bundleID);
            if (_bundlesByIdentifier) {
                bundlesWithThisID = (CFArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
                if (bundlesWithThisID && CFArrayGetCount(bundlesWithThisID) > 0) result = (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, 0);
            }
        }
        if (!result) {
            // Make sure all bundles have been created and try again.
            _CFBundleEnsureAllBundlesUpToDateAlreadyLocked();
            if (_bundlesByIdentifier) {
                bundlesWithThisID = (CFArrayRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
                if (bundlesWithThisID && CFArrayGetCount(bundlesWithThisID) > 0) result = (CFBundleRef)CFArrayGetValueAtIndex(bundlesWithThisID, 0);
            }
        }
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    }
    return result;
}

static CFStringRef __CFBundleCopyDescription(CFTypeRef cf) {
    char buff[CFMaxPathSize];
    CFStringRef path = NULL, binaryType = NULL, retval = NULL;
    if (((CFBundleRef)cf)->_url && CFURLGetFileSystemRepresentation(((CFBundleRef)cf)->_url, true, (uint8_t *)buff, CFMaxPathSize)) path = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, buff);
    switch (((CFBundleRef)cf)->_binaryType) {
        case __CFBundleCFMBinary:
            binaryType = CFSTR("");
            break;
        case __CFBundleDYLDExecutableBinary:
            binaryType = CFSTR("executable, ");
            break;
        case __CFBundleDYLDBundleBinary:
            binaryType = CFSTR("bundle, ");
            break;
        case __CFBundleDYLDFrameworkBinary:
            binaryType = CFSTR("framework, ");
            break;
        case __CFBundleDLLBinary:
            binaryType = CFSTR("DLL, ");
            break;
        case __CFBundleUnreadableBinary:
            binaryType = CFSTR("");
            break;
        default:
            binaryType = CFSTR("");
            break;
    }
    if (((CFBundleRef)cf)->_plugInData._isPlugIn) {
        retval = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("CFBundle/CFPlugIn %p <%@> (%@%sloaded)"), cf, path, binaryType, ((CFBundleRef)cf)->_isLoaded ? "" : "not ");
    } else {
        retval = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("CFBundle %p <%@> (%@%sloaded)"), cf, path, binaryType, ((CFBundleRef)cf)->_isLoaded ? "" : "not ");
    }
    if (path) CFRelease(path);
    return retval;
}

static void _CFBundleDeallocateGlue(const void *key, const void *value, void *context) {
    CFAllocatorRef allocator = (CFAllocatorRef)context;
    if (value) CFAllocatorDeallocate(allocator, (void *)value);
}

static void __CFBundleDeallocate(CFTypeRef cf) {
    CFBundleRef bundle = (CFBundleRef)cf;
    CFAllocatorRef allocator;
    
    __CFGenericValidateType(cf, __kCFBundleTypeID);

    allocator = CFGetAllocator(bundle);

    /* Unload it */
    CFBundleUnloadExecutable(bundle);

    // Clean up plugIn stuff
    _CFBundleDeallocatePlugIn(bundle);
    
    _CFBundleRemoveFromTables(bundle);

    if (bundle->_url) {
        _CFBundleFlushCachesForURL(bundle->_url);
        CFRelease(bundle->_url);
    }
    if (bundle->_infoDict) CFRelease(bundle->_infoDict);
    if (bundle->_modDate) CFRelease(bundle->_modDate);
    if (bundle->_localInfoDict) CFRelease(bundle->_localInfoDict);
    if (bundle->_searchLanguages) CFRelease(bundle->_searchLanguages);
    if (bundle->_glueDict) {
        CFDictionaryApplyFunction(bundle->_glueDict, _CFBundleDeallocateGlue, (void *)allocator);
        CFRelease(bundle->_glueDict);
    }
    if (bundle->_resourceData._stringTableCache) CFRelease(bundle->_resourceData._stringTableCache);
}

static const CFRuntimeClass __CFBundleClass = {
    0,
    "CFBundle",
    NULL,      // init
    NULL,      // copy
    __CFBundleDeallocate,
    NULL,      // equal
    NULL,      // hash
    NULL,      // 
    __CFBundleCopyDescription
};

__private_extern__ void __CFBundleInitialize(void) {
    __kCFBundleTypeID = _CFRuntimeRegisterClass(&__CFBundleClass);
#if defined(BINARY_SUPPORT_DLFCN)
    _useDlfcn = true;
#if defined(BINARY_SUPPORT_DYLD)
    if (getenv("CFBundleUseDYLD")) _useDlfcn = false;
#endif /* BINARY_SUPPORT_DYLD */
#endif /* BINARY_SUPPORT_DLFCN */
}

Boolean _CFBundleUseDlfcn(void) {
    return _useDlfcn;
}

CFTypeID CFBundleGetTypeID(void) {
    return __kCFBundleTypeID;
}

CFBundleRef _CFBundleGetExistingBundleWithBundleURL(CFURLRef bundleURL) {
    CFBundleRef bundle = NULL;
    char buff[CFMaxPathSize];
    CFURLRef newURL = NULL;
    
    if (!CFURLGetFileSystemRepresentation(bundleURL, true, (uint8_t *)buff, CFMaxPathSize)) return NULL;
    
    newURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)buff, (CFIndex)strlen(buff), true);
    if (!newURL) newURL = (CFURLRef)CFRetain(bundleURL);
    bundle = _CFBundleFindByURL(newURL, false);
    CFRelease(newURL);
    return bundle;
}

static CFBundleRef _CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL, Boolean alreadyLocked, Boolean doFinalProcessing) {
    CFBundleRef bundle = NULL;
    char buff[CFMaxPathSize];
    CFDateRef modDate = NULL;
    Boolean exists = false;
    SInt32 mode = 0;
    CFURLRef newURL = NULL;
    uint8_t localVersion = 0;
    
    if (!CFURLGetFileSystemRepresentation(bundleURL, true, (uint8_t *)buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(allocator, (uint8_t *)buff, (CFIndex)strlen(buff), true);
    if (!newURL) newURL = (CFURLRef)CFRetain(bundleURL);
    bundle = _CFBundleFindByURL(newURL, alreadyLocked);
    if (bundle) {
        CFRetain(bundle);
        CFRelease(newURL);
        return bundle;
    }
    
    if (!_CFBundleURLLooksLikeBundleVersion(newURL, &localVersion)) {
        localVersion = 3;
        if (_CFGetFileProperties(allocator, newURL, &exists, &mode, NULL, &modDate, NULL, NULL) == 0) {
            if (!exists || ((mode & S_IFMT) != S_IFDIR)) {
                if (modDate) CFRelease(modDate);
                CFRelease(newURL);
                return NULL;
            }
        } else {
            CFRelease(newURL);
            return NULL;
        }
    }

    bundle = (CFBundleRef)_CFRuntimeCreateInstance(allocator, __kCFBundleTypeID, sizeof(struct __CFBundle) - sizeof(CFRuntimeBase), NULL);
    if (!bundle) {
        CFRelease(newURL);
        return NULL;
    }

    bundle->_url = newURL;

    bundle->_modDate = modDate;
    bundle->_version = localVersion;
    bundle->_infoDict = NULL;
    bundle->_localInfoDict = NULL;
    bundle->_searchLanguages = NULL;
    
#if defined(BINARY_SUPPORT_DYLD)
    /* We'll have to figure it out later */
    bundle->_binaryType = __CFBundleUnknownBinary;
#elif defined(BINARY_SUPPORT_CFM)
    /* We support CFM only */
    bundle->_binaryType = __CFBundleCFMBinary;
#elif defined(BINARY_SUPPORT_DLL)
    /* We support DLL only */
    bundle->_binaryType = __CFBundleDLLBinary;
    bundle->_hModule = NULL;
#else
    /* We'll have to figure it out later */
    bundle->_binaryType = __CFBundleUnknownBinary;
#endif /* BINARY_SUPPORT_DYLD */

    bundle->_isLoaded = false;
    bundle->_sharesStringsFiles = false;
    
    if (!getenv("CFBundleDisableStringsSharing") && 
#if DEPLOYMENT_TARGET_MACOSX
        (strncmp(buff, "/System/Library/Frameworks", 26) == 0) && 
#endif
        (strncmp(buff + strlen(buff) - 10, ".framework", 10) == 0)) bundle->_sharesStringsFiles = true;

    bundle->_connectionCookie = NULL;
    bundle->_handleCookie = NULL;
    bundle->_imageCookie = NULL;
    bundle->_moduleCookie = NULL;

    bundle->_glueDict = NULL;
    
#if defined(BINARY_SUPPORT_CFM)
    bundle->_resourceData._executableLacksResourceFork = false;
#else /* BINARY_SUPPORT_CFM */
    bundle->_resourceData._executableLacksResourceFork = true;
#endif /* BINARY_SUPPORT_CFM */
    bundle->_resourceData._infoDictionaryFromResourceFork = false;
    bundle->_resourceData._stringTableCache = NULL;

    bundle->_plugInData._isPlugIn = false;
    bundle->_plugInData._loadOnDemand = false;
    bundle->_plugInData._isDoingDynamicRegistration = false;
    bundle->_plugInData._instanceCount = 0;
    bundle->_plugInData._factories = NULL;

    CFBundleGetInfoDictionary(bundle);
    
    _CFBundleAddToTables(bundle, alreadyLocked);

    if (doFinalProcessing) {
        _CFBundleCheckWorkarounds(bundle);
        if (_CFBundleNeedsInitPlugIn(bundle)) {
            if (alreadyLocked) __CFSpinUnlock(&CFBundleGlobalDataLock);
            _CFBundleInitPlugIn(bundle);
            if (alreadyLocked) __CFSpinLock(&CFBundleGlobalDataLock);
        }
    }
    
    return bundle;
}

CFBundleRef CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL) {return _CFBundleCreate(allocator, bundleURL, false, true);}

CFArrayRef CFBundleCreateBundlesFromDirectory(CFAllocatorRef alloc, CFURLRef directoryURL, CFStringRef bundleType) {
    CFMutableArrayRef bundles = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    CFArrayRef URLs = _CFContentsOfDirectory(alloc, NULL, NULL, directoryURL, bundleType);
    if (URLs) {
        CFIndex i, c = CFArrayGetCount(URLs);
        CFURLRef curURL;
        CFBundleRef curBundle;

        for (i = 0; i < c; i++) {
            curURL = (CFURLRef)CFArrayGetValueAtIndex(URLs, i);
            curBundle = CFBundleCreate(alloc, curURL);
            if (curBundle) CFArrayAppendValue(bundles, curBundle);
        }
        CFRelease(URLs);
    }

    return bundles;
}

CFURLRef CFBundleCopyBundleURL(CFBundleRef bundle) {
    if (bundle->_url) {
        CFRetain(bundle->_url);
    }
    return bundle->_url;
}

void _CFBundleSetDefaultLocalization(CFStringRef localizationName) {
    CFStringRef newLocalization = localizationName ? (CFStringRef)CFStringCreateCopy(kCFAllocatorSystemDefault, localizationName) : NULL;
    if (_defaultLocalization) CFRelease(_defaultLocalization);
    _defaultLocalization = newLocalization;
}

CFArrayRef _CFBundleGetLanguageSearchList(CFBundleRef bundle) {
    if (!bundle->_searchLanguages) {
        CFMutableArrayRef langs = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFStringRef devLang = CFBundleGetDevelopmentRegion(bundle);
        
        _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, devLang);

        if (CFArrayGetCount(langs) == 0) {
            // If the user does not prefer any of our languages, and devLang is not present, try English
            _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, CFSTR("en_US"));
        }
        if (CFArrayGetCount(langs) == 0) {
            // if none of the preferred localizations are present, fall back on a random localization that is present
            CFArrayRef localizations = CFBundleCopyBundleLocalizations(bundle);
            if (localizations) {
                if (CFArrayGetCount(localizations) > 0) {
                    _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, (CFStringRef)CFArrayGetValueAtIndex(localizations, 0));
                }
                CFRelease(localizations);
            }
        }
        
        if (devLang && !CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang)) {
            // Make sure that devLang is on the list as a fallback for individual resources that are not present
            CFArrayAppendValue(langs, devLang);
        } else if (!devLang) {
            // Or if there is no devLang, try some variation of English that is present
            CFArrayRef localizations = CFBundleCopyBundleLocalizations(bundle);
            if (localizations) {
                CFStringRef en_US = CFSTR("en_US"), en = CFSTR("en"), English = CFSTR("English");
                CFRange range = CFRangeMake(0, CFArrayGetCount(localizations));
                if (CFArrayContainsValue(localizations, range, en)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en)) CFArrayAppendValue(langs, en);
                } else if (CFArrayContainsValue(localizations, range, English)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), English)) CFArrayAppendValue(langs, English);
                } else if (CFArrayContainsValue(localizations, range, en_US)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en_US)) CFArrayAppendValue(langs, en_US);
                }
                CFRelease(localizations);
            }
        }
        if (CFArrayGetCount(langs) == 0) {
            // Total backstop behavior to avoid having an empty array.
            if (_defaultLocalization) {
                CFArrayAppendValue(langs, _defaultLocalization);
            } else {
                CFArrayAppendValue(langs, CFSTR("en"));
            }
        }
        bundle->_searchLanguages = langs;
    }
    return bundle->_searchLanguages;
}

CFDictionaryRef CFBundleCopyInfoDictionaryInDirectory(CFURLRef url) {return _CFBundleCopyInfoDictionaryInDirectory(kCFAllocatorSystemDefault, url, NULL);}

CFDictionaryRef CFBundleGetInfoDictionary(CFBundleRef bundle) {
    if (!bundle->_infoDict) bundle->_infoDict = _CFBundleCopyInfoDictionaryInDirectoryWithVersion(CFGetAllocator(bundle), bundle->_url, bundle->_version);
    return bundle->_infoDict;
}

CFDictionaryRef _CFBundleGetLocalInfoDictionary(CFBundleRef bundle) {return CFBundleGetLocalInfoDictionary(bundle);}

CFDictionaryRef CFBundleGetLocalInfoDictionary(CFBundleRef bundle) {
    if (!bundle->_localInfoDict) {
        CFURLRef url = CFBundleCopyResourceURL(bundle, _CFBundleLocalInfoName, _CFBundleStringTableType, NULL);
        if (url) {
            CFDataRef data;
            SInt32 errCode;
            CFStringRef errStr = NULL;
            
            if (CFURLCreateDataAndPropertiesFromResource(CFGetAllocator(bundle), url, &data, NULL, NULL, &errCode)) {
                bundle->_localInfoDict = (CFDictionaryRef)CFPropertyListCreateFromXMLData(CFGetAllocator(bundle), data, kCFPropertyListImmutable, &errStr);
                if (errStr) CFRelease(errStr);
                if (bundle->_localInfoDict && CFDictionaryGetTypeID() != CFGetTypeID(bundle->_localInfoDict)) {
                    CFRelease(bundle->_localInfoDict);
                    bundle->_localInfoDict = NULL;
                }
                CFRelease(data);
            }
            CFRelease(url);
        }
    }
    return bundle->_localInfoDict;
}

CFPropertyListRef _CFBundleGetValueForInfoKey(CFBundleRef bundle, CFStringRef key) {return (CFPropertyListRef)CFBundleGetValueForInfoDictionaryKey(bundle, key);}

CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef bundle, CFStringRef key) {
    // Look in InfoPlist.strings first.  Then look in Info.plist
    CFTypeRef result = NULL;
    if (bundle && key) {
        CFDictionaryRef dict = CFBundleGetLocalInfoDictionary(bundle);
        if (dict) result = CFDictionaryGetValue(dict, key);
        if (!result) {
            dict = CFBundleGetInfoDictionary(bundle);
            if (dict) result = CFDictionaryGetValue(dict, key);
        }
    }
    return result;
}

CFStringRef CFBundleGetIdentifier(CFBundleRef bundle) {
    CFStringRef bundleID = NULL;
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict) bundleID = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleIdentifierKey);
    return bundleID;
}

#define DEVELOPMENT_STAGE 0x20
#define ALPHA_STAGE 0x40
#define BETA_STAGE 0x60
#define RELEASE_STAGE 0x80

#define MAX_VERS_LEN 10

CF_INLINE Boolean _isDigit(UniChar aChar) {return (((aChar >= (UniChar)'0') && (aChar <= (UniChar)'9')) ? true : false);}

__private_extern__ CFStringRef _CFCreateStringFromVersionNumber(CFAllocatorRef alloc, UInt32 vers) {
    CFStringRef result = NULL;
    uint8_t major1, major2, minor1, minor2, stage, build;

    major1 = (vers & 0xF0000000) >> 28;
    major2 = (vers & 0x0F000000) >> 24;
    minor1 = (vers & 0x00F00000) >> 20;
    minor2 = (vers & 0x000F0000) >> 16;
    stage = (vers & 0x0000FF00) >> 8;
    build = (vers & 0x000000FF);

    if (stage == RELEASE_STAGE) {
        if (major1 > 0) {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d%d.%d.%d"), major1, major2, minor1, minor2);
        } else {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d.%d.%d"), major2, minor1, minor2);
        }
    } else {
        if (major1 > 0) {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d%d.%d.%d%s%d"), major1, major2, minor1, minor2, ((stage == DEVELOPMENT_STAGE) ? "d" : ((stage == ALPHA_STAGE) ? "a" : "b")), build);
        } else {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d.%d.%d%s%d"), major2, minor1, minor2, ((stage == DEVELOPMENT_STAGE) ? "d" : ((stage == ALPHA_STAGE) ? "a" : "b")), build);
        }
    }
    return result;
}

__private_extern__ UInt32 _CFVersionNumberFromString(CFStringRef versStr) {
    // Parse version number from string.
    // String can begin with "." for major version number 0.  String can end at any point, but elements within the string cannot be skipped.
    UInt32 major1 = 0, major2 = 0, minor1 = 0, minor2 = 0, stage = RELEASE_STAGE, build = 0;
    UniChar versChars[MAX_VERS_LEN];
    UniChar *chars = NULL;
    CFIndex len;
    UInt32 theVers;
    Boolean digitsDone = false;

    if (!versStr) return 0;

    len = CFStringGetLength(versStr);

    if ((len == 0) || (len > MAX_VERS_LEN)) return 0;

    CFStringGetCharacters(versStr, CFRangeMake(0, len), versChars);
    chars = versChars;
    
    // Get major version number.
    major1 = major2 = 0;
    if (_isDigit(*chars)) {
        major2 = *chars - (UniChar)'0';
        chars++;
        len--;
        if (len > 0) {
            if (_isDigit(*chars)) {
                major1 = major2;
                major2 = *chars - (UniChar)'0';
                chars++;
                len--;
                if (len > 0) {
                    if (*chars == (UniChar)'.') {
                        chars++;
                        len--;
                    } else {
                        digitsDone = true;
                    }
                }
            } else if (*chars == (UniChar)'.') {
                chars++;
                len--;
            } else {
                digitsDone = true;
            }
        }
    } else if (*chars == (UniChar)'.') {
        chars++;
        len--;
    } else {
        digitsDone = true;
    }

    // Now major1 and major2 contain first and second digit of the major version number as ints.
    // Now either len is 0 or chars points at the first char beyond the first decimal point.

    // Get the first minor version number.  
    if (len > 0 && !digitsDone) {
        if (_isDigit(*chars)) {
            minor1 = *chars - (UniChar)'0';
            chars++;
            len--;
            if (len > 0) {
                if (*chars == (UniChar)'.') {
                    chars++;
                    len--;
                } else {
                    digitsDone = true;
                }
            }
        } else {
            digitsDone = true;
        }
    }

    // Now minor1 contains the first minor version number as an int.
    // Now either len is 0 or chars points at the first char beyond the second decimal point.

    // Get the second minor version number. 
    if (len > 0 && !digitsDone) {
        if (_isDigit(*chars)) {
            minor2 = *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            digitsDone = true;
        }
    }

    // Now minor2 contains the second minor version number as an int.
    // Now either len is 0 or chars points at the build stage letter.

    // Get the build stage letter.  We must find 'd', 'a', 'b', or 'f' next, if there is anything next.
    if (len > 0) {
        if (*chars == (UniChar)'d') {
            stage = DEVELOPMENT_STAGE;
        } else if (*chars == (UniChar)'a') {
            stage = ALPHA_STAGE;
        } else if (*chars == (UniChar)'b') {
            stage = BETA_STAGE;
        } else if (*chars == (UniChar)'f') {
            stage = RELEASE_STAGE;
        } else {
            return 0;
        }
        chars++;
        len--;
    }

    // Now stage contains the release stage.
    // Now either len is 0 or chars points at the build number.

    // Get the first digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build = *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }
    // Get the second digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build *= 10;
            build += *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }
    // Get the third digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build *= 10;
            build += *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }

    // Range check the build number and make sure we exhausted the string.
    if ((build > 0xFF) || (len > 0)) return 0;

    // Build the number
    theVers = major1 << 28;
    theVers += major2 << 24;
    theVers += minor1 << 20;
    theVers += minor2 << 16;
    theVers += stage << 8;
    theVers += build;

    return theVers;
}

UInt32 CFBundleGetVersionNumber(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFTypeRef unknownVersionValue = CFDictionaryGetValue(infoDict, _kCFBundleNumericVersionKey);
    CFNumberRef versNum;
    UInt32 vers = 0;

    if (!unknownVersionValue) unknownVersionValue = CFDictionaryGetValue(infoDict, kCFBundleVersionKey);
    if (unknownVersionValue) {
        if (CFGetTypeID(unknownVersionValue) == CFStringGetTypeID()) {
            // Convert a string version number into a numeric one.
            vers = _CFVersionNumberFromString((CFStringRef)unknownVersionValue);

            versNum = CFNumberCreate(CFGetAllocator(bundle), kCFNumberSInt32Type, &vers);
            CFDictionarySetValue((CFMutableDictionaryRef)infoDict, _kCFBundleNumericVersionKey, versNum);
            CFRelease(versNum);
        } else if (CFGetTypeID(unknownVersionValue) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)unknownVersionValue, kCFNumberSInt32Type, &vers);
        } else {
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, _kCFBundleNumericVersionKey);
        }
    }
    return vers;
}

CFStringRef CFBundleGetDevelopmentRegion(CFBundleRef bundle) {
    CFStringRef devLang = NULL;
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict) {
        devLang = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
        if (devLang && (CFGetTypeID(devLang) != CFStringGetTypeID() || CFStringGetLength(devLang) == 0)) {
            devLang = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleDevelopmentRegionKey);
        }
    }

    return devLang;
}

Boolean _CFBundleGetHasChanged(CFBundleRef bundle) {
    CFDateRef modDate;
    Boolean result = false;
    Boolean exists = false;
    SInt32 mode = 0;

    if (_CFGetFileProperties(CFGetAllocator(bundle), bundle->_url, &exists, &mode, NULL, &modDate, NULL, NULL) == 0) {
        // If the bundle no longer exists or is not a folder, it must have "changed"
        if (!exists || ((mode & S_IFMT) != S_IFDIR)) result = true;
    } else {
        // Something is wrong.  The stat failed.
        result = true;
    }
    if (bundle->_modDate && !CFEqual(bundle->_modDate, modDate)) {
        // mod date is different from when we created.
        result = true;
    }
    CFRelease(modDate);
    return result;
}

void _CFBundleSetStringsFilesShared(CFBundleRef bundle, Boolean flag) {
    bundle->_sharesStringsFiles = flag;
}

Boolean _CFBundleGetStringsFilesShared(CFBundleRef bundle) {
    return bundle->_sharesStringsFiles;
}

static Boolean _urlExists(CFAllocatorRef alloc, CFURLRef url) {
    Boolean exists;
    return url && (0 == _CFGetFileProperties(alloc, url, &exists, NULL, NULL, NULL, NULL, NULL)) && exists;
}

__private_extern__ CFURLRef _CFBundleCopySupportFilesDirectoryURLInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version) {
    CFURLRef result = NULL;
    if (bundleURL) {
        if (1 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleSupportFilesURLFromBase1, bundleURL);
        } else if (2 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleSupportFilesURLFromBase2, bundleURL);
        } else {
            result = (CFURLRef)CFRetain(bundleURL);
        }
    }
    return result;
}

CF_EXPORT CFURLRef CFBundleCopySupportFilesDirectoryURL(CFBundleRef bundle) {return _CFBundleCopySupportFilesDirectoryURLInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version);}

__private_extern__ CFURLRef _CFBundleCopyResourcesDirectoryURLInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version) {
    CFURLRef result = NULL;
    if (bundleURL) {
        if (0 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase0, bundleURL);
        } else if (1 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase1, bundleURL);
        } else if (2 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase2, bundleURL);
        } else {
            result = (CFURLRef)CFRetain(bundleURL);
        }
    }
    return result;
}

CFURLRef CFBundleCopyResourcesDirectoryURL(CFBundleRef bundle) {return _CFBundleCopyResourcesDirectoryURLInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version);}

static CFURLRef _CFBundleCopyExecutableURLRaw(CFAllocatorRef alloc, CFURLRef urlPath, CFStringRef exeName) {
    // Given an url to a folder and a name, this returns the url to the executable in that folder with that name, if it exists, and NULL otherwise.  This function deals with appending the ".exe" or ".dll" on Windows.
    CFURLRef executableURL = NULL;
    if (!urlPath || !exeName) return NULL;
    
#if DEPLOYMENT_TARGET_MACOSX
    const uint8_t *image_suffix = (uint8_t *)getenv("DYLD_IMAGE_SUFFIX");
    if (image_suffix) {
        CFStringRef newExeName, imageSuffix;
        imageSuffix = CFStringCreateWithCString(kCFAllocatorSystemDefault, (char *)image_suffix, kCFStringEncodingUTF8);
        if (CFStringHasSuffix(exeName, CFSTR(".dylib"))) {
            CFStringRef bareExeName = CFStringCreateWithSubstring(alloc, exeName, CFRangeMake(0, CFStringGetLength(exeName)-6));
            newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@.dylib"), exeName, imageSuffix);
            CFRelease(bareExeName);
        } else {
            newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, imageSuffix);
        }
        executableURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, newExeName, kCFURLPOSIXPathStyle, false, urlPath);
        if (executableURL && !_urlExists(alloc, executableURL)) {
            CFRelease(executableURL);
            executableURL = NULL;
        }
        CFRelease(newExeName);
        CFRelease(imageSuffix);
    }
#endif
    if (!executableURL) {
        executableURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, exeName, kCFURLPOSIXPathStyle, false, urlPath);
        if (executableURL && !_urlExists(alloc, executableURL)) {
            CFRelease(executableURL);
            executableURL = NULL;
        }
    }
#if defined(DEPLOYMENT_TARGET_WINDOWS)
    if (executableURL == NULL) {
        if (!CFStringHasSuffix(exeName, CFSTR(".dll"))) {
            CFStringRef newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, CFSTR(".dll"));
            executableURL = CFURLCreateWithString(alloc, newExeName, urlPath);
            if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
                CFRelease(executableURL);
                executableURL = NULL;
            }
            CFRelease(newExeName);
        }
    }
    if (executableURL == NULL) {
        if (!CFStringHasSuffix(exeName, CFSTR(".exe"))) {
            CFStringRef newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, CFSTR(".exe"));
            executableURL = CFURLCreateWithString(alloc, newExeName, urlPath);
            if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
                CFRelease(executableURL);
                executableURL = NULL;
            }
            CFRelease(newExeName);
        }
    }
#endif
    return executableURL;
}

static CFStringRef _CFBundleCopyExecutableName(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFDictionaryRef infoDict) {
    CFStringRef executableName = NULL;
    
    if (!alloc && bundle) alloc = CFGetAllocator(bundle);
    if (!infoDict && bundle) infoDict = CFBundleGetInfoDictionary(bundle);
    if (!url && bundle) url = bundle->_url;
    
    if (infoDict) {
        // Figure out the name of the executable.
        // First try for the new key in the plist.
        executableName = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleExecutableKey);
        // Second try for the old key in the plist.
        if (!executableName) executableName = (CFStringRef)CFDictionaryGetValue(infoDict, _kCFBundleOldExecutableKey);
        if (executableName && CFGetTypeID(executableName) == CFStringGetTypeID() && CFStringGetLength(executableName) > 0) {
            CFRetain(executableName);
        } else {
            executableName = NULL;
        }
    }
    if (!executableName && url) {
        // Third, take the name of the bundle itself (with path extension stripped)
        CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
        CFStringRef bundlePath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
        UniChar buff[CFMaxPathSize];
        CFIndex len = CFStringGetLength(bundlePath);
        CFIndex startOfBundleName, endOfBundleName;

        CFRelease(absoluteURL);
        if (len > CFMaxPathSize) len = CFMaxPathSize;
        CFStringGetCharacters(bundlePath, CFRangeMake(0, len), buff);
        startOfBundleName = _CFStartOfLastPathComponent(buff, len);
        endOfBundleName = _CFLengthAfterDeletingPathExtension(buff, len);

        if ((startOfBundleName <= len) && (endOfBundleName <= len) && (startOfBundleName < endOfBundleName)) {
            executableName = CFStringCreateWithCharacters(alloc, &(buff[startOfBundleName]), (endOfBundleName - startOfBundleName));
        }
        CFRelease(bundlePath);
    }
    
    return executableName;
}

__private_extern__ CFURLRef _CFBundleCopyResourceForkURLMayBeLocal(CFBundleRef bundle, Boolean mayBeLocal) {
    CFStringRef executableName = _CFBundleCopyExecutableName(kCFAllocatorSystemDefault, bundle, NULL, NULL);
    CFURLRef resourceForkURL = NULL;
    if (executableName) {
        if (mayBeLocal) {
            resourceForkURL = CFBundleCopyResourceURL(bundle, executableName, CFSTR("rsrc"), NULL);
        } else {
            resourceForkURL = CFBundleCopyResourceURLForLocalization(bundle, executableName, CFSTR("rsrc"), NULL, NULL);
        }
        CFRelease(executableName);
    }
    
    return resourceForkURL;
}

CFURLRef _CFBundleCopyResourceForkURL(CFBundleRef bundle) {return _CFBundleCopyResourceForkURLMayBeLocal(bundle, true);}

static CFURLRef _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFStringRef executableName, Boolean ignoreCache, Boolean useOtherPlatform) {
    uint8_t version = 0;
    CFDictionaryRef infoDict = NULL;
    CFStringRef executablePath = NULL;
    CFURLRef executableURL = NULL;
    Boolean foundIt = false;
    Boolean lookupMainExe = (executableName ? false : true);
    
    if (bundle) {
        infoDict = CFBundleGetInfoDictionary(bundle);
        version = bundle->_version;
    } else {
        infoDict = _CFBundleCopyInfoDictionaryInDirectory(alloc, url, &version);
    }

    // If we have a bundle instance and an info dict, see if we have already cached the path
    if (lookupMainExe && !ignoreCache && !useOtherPlatform && bundle && infoDict) {
        executablePath = (CFStringRef)CFDictionaryGetValue(infoDict, _kCFBundleExecutablePathKey);
        if (executablePath) {
#if DEPLOYMENT_TARGET_MACOSX
            executableURL = CFURLCreateWithFileSystemPath(alloc, executablePath, kCFURLPOSIXPathStyle, false);
#else
            executableURL = CFURLCreateWithFileSystemPath(alloc, executablePath, kCFURLWindowsPathStyle, false);
#endif
            if (executableURL) foundIt = true;
            if (!foundIt) {
                executablePath = NULL;
                CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, _kCFBundleExecutablePathKey);
            }
        }
    }

    if (!foundIt) {
        if (lookupMainExe) {
            executableName = _CFBundleCopyExecutableName(alloc, bundle, url, infoDict);
        }
        if (executableName) {
            Boolean doExecSearch = true;
            // Now, look for the executable inside the bundle.
            if (doExecSearch && 0 != version) {
                CFURLRef exeDirURL;
                CFURLRef exeSubdirURL;

                if (1 == version) {
                    exeDirURL = CFURLCreateWithString(alloc, _CFBundleExecutablesURLFromBase1, url);
                } else if (2 == version) {
                    exeDirURL = CFURLCreateWithString(alloc, _CFBundleExecutablesURLFromBase2, url);
                } else {
                    exeDirURL = (CFURLRef)CFRetain(url);
                }
                CFStringRef platformSubDir = useOtherPlatform ? _CFBundleGetOtherPlatformExecutablesSubdirectoryName() : _CFBundleGetPlatformExecutablesSubdirectoryName();
                exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, platformSubDir, kCFURLPOSIXPathStyle, true, exeDirURL);
                executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                if (!executableURL) {
                    CFRelease(exeSubdirURL);
                    platformSubDir = useOtherPlatform ? _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName() : _CFBundleGetAlternatePlatformExecutablesSubdirectoryName();
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, platformSubDir, kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (!executableURL) {
                    CFRelease(exeSubdirURL);
                    platformSubDir = useOtherPlatform ? _CFBundleGetPlatformExecutablesSubdirectoryName() : _CFBundleGetOtherPlatformExecutablesSubdirectoryName();
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, platformSubDir, kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (!executableURL) {
                    CFRelease(exeSubdirURL);
                    platformSubDir = useOtherPlatform ? _CFBundleGetAlternatePlatformExecutablesSubdirectoryName() : _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName();
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, platformSubDir, kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (!executableURL) {
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeDirURL, executableName);
                }

                CFRelease(exeDirURL);
                CFRelease(exeSubdirURL);
            }

#if defined(DEPLOYMENT_TARGET_WINDOWS)
            // Windows only: If we still haven't found the exe, look in the Executables folder.
            // But only for the main bundle exe
            if (lookupMainExe && (executableURL == NULL)) {
                CFURLRef exeDirURL;

                exeDirURL = CFURLCreateWithString(alloc, CFSTR("../../Executables"), url);

                executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeDirURL, executableName);

                CFRelease(exeDirURL);
            }
#endif

            // If this was an old bundle, or we did not find the executable in the Excutables subdirectory, look directly in the bundle wrapper.
            if (!executableURL) executableURL = _CFBundleCopyExecutableURLRaw(alloc, url, executableName);
            if (lookupMainExe && !ignoreCache && !useOtherPlatform && bundle && infoDict && executableURL) {
                // We found it.  Cache the path.
                CFURLRef absURL = CFURLCopyAbsoluteURL(executableURL);
#if DEPLOYMENT_TARGET_MACOSX
                executablePath = CFURLCopyFileSystemPath(absURL, kCFURLPOSIXPathStyle);
#else
                executablePath = CFURLCopyFileSystemPath(absURL, kCFURLWindowsPathStyle);
#endif
                CFRelease(absURL);
                CFDictionarySetValue((CFMutableDictionaryRef)infoDict, _kCFBundleExecutablePathKey, executablePath);
                CFRelease(executablePath);
            }
            if (lookupMainExe && !useOtherPlatform && bundle && !executableURL) bundle->_binaryType = __CFBundleNoBinary;
            if (lookupMainExe) CFRelease(executableName);
        }
    }

    if (!bundle && infoDict) CFRelease(infoDict);

    return executableURL;
}

CFURLRef _CFBundleCopyExecutableURLInDirectory(CFURLRef url) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(kCFAllocatorSystemDefault, NULL, url, NULL, true, false);}

CFURLRef _CFBundleCopyOtherExecutableURLInDirectory(CFURLRef url) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(kCFAllocatorSystemDefault, NULL, url, NULL, true, true);}

CFURLRef CFBundleCopyExecutableURL(CFBundleRef bundle) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, NULL, false, false);}

static CFURLRef _CFBundleCopyExecutableURLIgnoringCache(CFBundleRef bundle) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, NULL, true, false);}

CFURLRef CFBundleCopyAuxiliaryExecutableURL(CFBundleRef bundle, CFStringRef executableName) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, executableName, true, false);}

Boolean CFBundleIsExecutableLoaded(CFBundleRef bundle) {return bundle->_isLoaded;}

CFBundleExecutableType CFBundleGetExecutableType(CFBundleRef bundle) {
    CFBundleExecutableType result = kCFBundleOtherExecutableType;
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

    if (!executableURL) bundle->_binaryType = __CFBundleNoBinary;
#if defined(BINARY_SUPPORT_DYLD)
    if (bundle->_binaryType == __CFBundleUnknownBinary) {
        bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
        if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) bundle->_resourceData._executableLacksResourceFork = true;
#endif /* BINARY_SUPPORT_CFM */
    }
#endif /* BINARY_SUPPORT_DYLD */
    if (executableURL) CFRelease(executableURL);

    if (bundle->_binaryType == __CFBundleCFMBinary) {
        result = kCFBundlePEFExecutableType;
    } else if (bundle->_binaryType == __CFBundleDYLDExecutableBinary || bundle->_binaryType == __CFBundleDYLDBundleBinary || bundle->_binaryType == __CFBundleDYLDFrameworkBinary) {
        result = kCFBundleMachOExecutableType;
    } else if (bundle->_binaryType == __CFBundleDLLBinary) {
        result = kCFBundleDLLExecutableType;
    } else if (bundle->_binaryType == __CFBundleELFBinary) {
        result = kCFBundleELFExecutableType;    
    }
    return result;
}

#define UNKNOWN_FILETYPE 0x0
#define PEF_FILETYPE 0x1000
#define PEF_MAGIC 0x4a6f7921
#define PEF_CIGAM 0x21796f4a
#define TEXT_SEGMENT "__TEXT"
#define PLIST_SECTION "__info_plist"
#define OBJC_SEGMENT "__OBJC"
#define IMAGE_INFO_SECTION "__image_info"
#define LIB_X11 "/usr/X11R6/lib/libX"

#define XLS_NAME "Book"
#define XLS_NAME2 "Workbook"
#define DOC_NAME "WordDocument"
#define PPT_NAME "PowerPoint Document"

#define ustrncmp(x, y, z) strncmp((char *)(x), (char *)(y), (z))
#if DEPLOYMENT_TARGET_WINDOWS
#if _MSC_VER
#define ustrncasecmp(x, y, z) _strnicmp_l((char *)(x), (char *)(y), (z), NULL)
#else
#define ustrncasecmp(x, y, z) strncasecmp((char *)(x), (char *)(y), (z))
#endif
#elif DEPLOYMENT_TARGET_LINUX
// When compiling with -Wall the GNU C library complains about passing
// NULL to strncasecmp_l as the locale. Since a locale is never
// passed, the C locale version should be a suitable replacement.
#define ustrncasecmp(x, y, z) strncasecmp((char *)(x), (char *)(y), (z))
#else
#define ustrncasecmp(x, y, z) strncasecmp_l((char *)(x), (char *)(y), (z), NULL)
#endif

static const uint32_t __CFBundleMagicNumbersArray[] = {
    0xcafebabe, 0xbebafeca, 0xfeedface, 0xcefaedfe, 0xfeedfacf, 0xcffaedfe, 0x4a6f7921, 0x21796f4a, 
    0x7f454c46, 0xffd8ffe0, 0x4d4d002a, 0x49492a00, 0x47494638, 0x89504e47, 0x69636e73, 0x00000100, 
    0x7b5c7274, 0x25504446, 0x2e7261fd, 0x2e524d46, 0x2e736e64, 0x2e736400, 0x464f524d, 0x52494646, 
    0x38425053, 0x000001b3, 0x000001ba, 0x4d546864, 0x504b0304, 0x53495421, 0x53495432, 0x53495435, 
    0x53495444, 0x53747566, 0x30373037, 0x3c212d2d, 0x25215053, 0xd0cf11e0, 0x62656769, 0x3d796265,
    0x6b6f6c79, 0x3026b275, 0x0000000c, 0xfe370023, 0x09020600, 0x09040600, 0x4f676753, 0x664c6143, 
    0x00010000, 0x74727565, 0x4f54544f, 0x41433130, 0xc809fe02, 0x0809fe02, 0x2356524d, 0x67696d70, 
    0x3c435058, 0x28445746, 0x424f4d53, 0x49544f4c, 0x72746664
};

// string, with groups of 5 characters being 1 element in the array
static const char * __CFBundleExtensionsArray =
    "mach\0"  "mach\0"  "mach\0"  "mach\0"  "mach\0"  "mach\0"  "pef\0\0" "pef\0\0" 
    "elf\0\0" "jpeg\0"  "tiff\0"  "tiff\0"  "gif\0\0" "png\0\0" "icns\0"  "ico\0\0" 
    "rtf\0\0" "pdf\0\0" "ra\0\0\0""rm\0\0\0""au\0\0\0""au\0\0\0""iff\0\0" "riff\0"  
    "psd\0\0" "mpeg\0"  "mpeg\0"  "mid\0\0" "zip\0\0" "sit\0\0" "sit\0\0" "sit\0\0" 
    "sit\0\0" "sit\0\0" "cpio\0"  "html\0"  "ps\0\0\0""ole\0\0" "uu\0\0\0""ync\0\0"
    "dmg\0\0" "wmv\0\0" "jp2\0\0" "doc\0\0" "xls\0\0" "xls\0\0" "ogg\0\0" "flac\0"
    "ttf\0\0" "ttf\0\0" "otf\0\0" "dwg\0\0" "dgn\0\0" "dgn\0\0" "wrl\0\0" "xcf\0\0"
    "cpx\0\0" "dwf\0\0" "bom\0\0" "lit\0\0" "rtfd\0";

static const char * __CFBundleOOExtensionsArray = "sxc\0\0" "sxd\0\0" "sxg\0\0" "sxi\0\0" "sxm\0\0" "sxw\0\0";
static const char * __CFBundleODExtensionsArray = "odc\0\0" "odf\0\0" "odg\0\0" "oth\0\0" "odi\0\0" "odm\0\0" "odp\0\0" "ods\0\0" "odt\0\0";

#define EXTENSION_LENGTH                5
#define NUM_EXTENSIONS                  61
#define MAGIC_BYTES_TO_READ             512
#define DMG_BYTES_TO_READ               512
#define ZIP_BYTES_TO_READ               1024
#define OLE_BYTES_TO_READ               512
#define X11_BYTES_TO_READ               4096
#define IMAGE_INFO_BYTES_TO_READ        4096

#if defined(BINARY_SUPPORT_DYLD)

CF_INLINE uint32_t _CFBundleSwapInt32Conditional(uint32_t arg, Boolean swap) {return swap ? CFSwapInt32(arg) : arg;}
CF_INLINE uint32_t _CFBundleSwapInt64Conditional(uint64_t arg, Boolean swap) {return swap ? CFSwapInt64(arg) : arg;}

static CFDictionaryRef _CFBundleGrokInfoDictFromData(const char *bytes, uint32_t length) {
    CFMutableDictionaryRef result = NULL;
    CFDataRef infoData = NULL;
    if (bytes && 0 < length) {
        infoData = CFDataCreateWithBytesNoCopy(kCFAllocatorSystemDefault, (uint8_t *)bytes, length, kCFAllocatorNull);
        if (infoData) {
            result = (CFMutableDictionaryRef)CFPropertyListCreateFromXMLData(kCFAllocatorSystemDefault, infoData, kCFPropertyListMutableContainers, NULL);
            if (result && CFDictionaryGetTypeID() != CFGetTypeID(result)) {
                CFRelease(result);
                result = NULL;
            }
            CFRelease(infoData);
        }
        if (!result) result = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    return result;
}

static CFDictionaryRef _CFBundleGrokInfoDictFromMainExecutable() {
    unsigned long length = 0;
    char *bytes = getsectdata(TEXT_SEGMENT, PLIST_SECTION, &length);
    return _CFBundleGrokInfoDictFromData(bytes, length);
}

static Boolean _CFBundleGrokObjCImageInfoFromMainExecutable(uint32_t *objcVersion, uint32_t *objcFlags) {
    Boolean retval = false;
    uint32_t localVersion = 0, localFlags = 0;
    if (getsegbyname(OBJC_SEGMENT)) {
        unsigned long length = 0;
        char *bytes = getsectdata(OBJC_SEGMENT, IMAGE_INFO_SECTION, &length);
        if (bytes && length >= 8) {
            localVersion = *(uint32_t *)bytes;
            localFlags = *(uint32_t *)(bytes + 4);
        }
        retval = true;
    }
    if (objcVersion) *objcVersion = localVersion;
    if (objcFlags) *objcFlags = localFlags;
    return retval;
}

static Boolean _CFBundleGrokX11FromFile(int fd, const void *bytes, CFIndex length, uint32_t offset, Boolean swapped, Boolean sixtyFour) {
    static const char libX11name[] = LIB_X11;
    char *buffer = NULL;
    const char *loc = NULL;
    unsigned i;
    Boolean result = false;
    
    if (fd >= 0 && lseek(fd, offset, SEEK_SET) == (off_t)offset) {
        buffer = (char*)malloc(X11_BYTES_TO_READ);
        if (buffer && read(fd, buffer, X11_BYTES_TO_READ) >= X11_BYTES_TO_READ) loc = buffer;
    } else if (bytes && length >= offset + X11_BYTES_TO_READ) {
        loc = (const char*)bytes + offset;
    }
    if (loc) {
        if (sixtyFour) {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)loc)->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)loc)->sizeofcmds, swapped);
            const char *startofcmds = loc + sizeof(struct mach_header_64);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct dylib_command *dlp = (struct dylib_command *)startofcmds;
            if (endofcmds > loc + X11_BYTES_TO_READ) endofcmds = loc + X11_BYTES_TO_READ;
            for (i = 0; !result && i < ncmds && startofcmds <= (char *)dlp && (char *)dlp < endofcmds; i++) {
                if (LC_LOAD_DYLIB == _CFBundleSwapInt32Conditional(dlp->cmd, swapped)) {
                    uint32_t nameoffset = _CFBundleSwapInt32Conditional(dlp->dylib.name.offset, swapped);
                    const char *name = (const char *)dlp + nameoffset;
                    if (startofcmds <= name && name + sizeof(libX11name) <= endofcmds && 0 == strncmp(name, libX11name, sizeof(libX11name) - 1)) result = true;
                }
                dlp = (struct dylib_command *)((char *)dlp + _CFBundleSwapInt32Conditional(dlp->cmdsize, swapped));
            }
        } else {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header *)loc)->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header *)loc)->sizeofcmds, swapped);
            const char *startofcmds = loc + sizeof(struct mach_header);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct dylib_command *dlp = (struct dylib_command *)startofcmds;
            if (endofcmds > loc + X11_BYTES_TO_READ) endofcmds = loc + X11_BYTES_TO_READ;
            for (i = 0; !result && i < ncmds && startofcmds <= (char *)dlp && (char *)dlp < endofcmds; i++) {
                if (LC_LOAD_DYLIB == _CFBundleSwapInt32Conditional(dlp->cmd, swapped)) {
                    uint32_t nameoffset = _CFBundleSwapInt32Conditional(dlp->dylib.name.offset, swapped);
                    const char *name = (const char *)dlp + nameoffset;
                    if (startofcmds <= name && name + sizeof(libX11name) <= endofcmds && 0 == strncmp(name, libX11name, sizeof(libX11name) - 1)) result = true;
                }
                dlp = (struct dylib_command *)((char *)dlp + _CFBundleSwapInt32Conditional(dlp->cmdsize, swapped));
            }
        }
    }
    
    if (buffer) free(buffer);
    
    return result;
}
    
static CFDictionaryRef _CFBundleGrokInfoDictFromFile(int fd, const void *bytes, CFIndex length, uint32_t offset, Boolean swapped, Boolean sixtyFour) {
    struct stat statBuf;
    off_t fileLength = 0;
    char *maploc = NULL;
    const char *loc;
    unsigned i, j;
    CFDictionaryRef result = NULL;
    Boolean foundit = false;
    if (fd >= 0 && fstat(fd, &statBuf) == 0 && (maploc = (char*)mmap(0, statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) != (void *)-1) {
        loc = maploc;
        fileLength = statBuf.st_size;
    } else {
        loc = (const char*)bytes;
        fileLength = length;
    }
    if (fileLength > offset + sizeof(struct mach_header_64)) {
        if (sixtyFour) {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)(loc + offset))->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)(loc + offset))->sizeofcmds, swapped);
            const char *startofcmds = loc + offset + sizeof(struct mach_header_64);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct segment_command_64 *sgp = (struct segment_command_64 *)startofcmds;
            if (endofcmds > loc + fileLength) endofcmds = loc + fileLength;
            for (i = 0; !foundit && i < ncmds && startofcmds <= (char *)sgp && (char *)sgp < endofcmds; i++) {
                if (LC_SEGMENT_64 == _CFBundleSwapInt32Conditional(sgp->cmd, swapped)) {
                    struct section_64 *sp = (struct section_64 *)((char *)sgp + sizeof(struct segment_command_64));
                    uint32_t nsects = _CFBundleSwapInt32Conditional(sgp->nsects, swapped);
                    for (j = 0; !foundit && j < nsects && startofcmds <= (char *)sp && (char *)sp < endofcmds; j++) {
                        if (0 == strncmp(sp->sectname, PLIST_SECTION, sizeof(sp->sectname)) && 0 == strncmp(sp->segname, TEXT_SEGMENT, sizeof(sp->segname))) {
                            uint64_t sectlength64 = _CFBundleSwapInt64Conditional(sp->size, swapped);
                            uint32_t sectlength = (uint32_t)(sectlength64 & 0xffffffff);
                            uint32_t sectoffset = _CFBundleSwapInt32Conditional(sp->offset, swapped);
                            const char *sectbytes = loc + offset + sectoffset;
                            // we don't support huge-sized plists
                            if (sectlength64 <= 0xffffffff && loc <= sectbytes && sectbytes + sectlength <= loc + fileLength) result = _CFBundleGrokInfoDictFromData(sectbytes, sectlength);
                            foundit = true;
                        }
                        sp = (struct section_64 *)((char *)sp + sizeof(struct section_64));
                    }
                }
                sgp = (struct segment_command_64 *)((char *)sgp + _CFBundleSwapInt32Conditional(sgp->cmdsize, swapped));
            }
        } else {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(loc + offset))->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(loc + offset))->sizeofcmds, swapped);
            const char *startofcmds = loc + offset + sizeof(struct mach_header);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct segment_command *sgp = (struct segment_command *)startofcmds;
            if (endofcmds > loc + fileLength) endofcmds = loc + fileLength;
            for (i = 0; !foundit && i < ncmds && startofcmds <= (char *)sgp && (char *)sgp < endofcmds; i++) {
                if (LC_SEGMENT == _CFBundleSwapInt32Conditional(sgp->cmd, swapped)) {
                    struct section *sp = (struct section *)((char *)sgp + sizeof(struct segment_command));
                    uint32_t nsects = _CFBundleSwapInt32Conditional(sgp->nsects, swapped);
                    for (j = 0; !foundit && j < nsects && startofcmds <= (char *)sp && (char *)sp < endofcmds; j++) {
                        if (0 == strncmp(sp->sectname, PLIST_SECTION, sizeof(sp->sectname)) && 0 == strncmp(sp->segname, TEXT_SEGMENT, sizeof(sp->segname))) {
                            uint32_t sectlength = _CFBundleSwapInt32Conditional(sp->size, swapped);
                            uint32_t sectoffset = _CFBundleSwapInt32Conditional(sp->offset, swapped);
                            const char *sectbytes = loc + offset + sectoffset;
                            if (loc <= sectbytes && sectbytes + sectlength <= loc + fileLength) result = _CFBundleGrokInfoDictFromData(sectbytes, sectlength);
                            foundit = true;
                        }
                        sp = (struct section *)((char *)sp + sizeof(struct section));
                    }
                }
                sgp = (struct segment_command *)((char *)sgp + _CFBundleSwapInt32Conditional(sgp->cmdsize, swapped));
            }
        }
    }
    if (maploc) munmap(maploc, statBuf.st_size);
    return result;
}

static void _CFBundleGrokObjcImageInfoFromFile(int fd, const void *bytes, CFIndex length, uint32_t offset, Boolean swapped, Boolean sixtyFour, Boolean *hasObjc, uint32_t *objcVersion, uint32_t *objcFlags) {
    uint32_t sectlength = 0, sectoffset = 0, localVersion = 0, localFlags = 0;
    char *buffer = NULL;
    char sectbuffer[8];
    const char *loc = NULL;
    unsigned i, j;
    Boolean foundit = false, localHasObjc = false;
    
    if (fd >= 0 && lseek(fd, offset, SEEK_SET) == (off_t)offset) {
        buffer = (char*)malloc(IMAGE_INFO_BYTES_TO_READ);
        if (buffer && read(fd, buffer, IMAGE_INFO_BYTES_TO_READ) >= IMAGE_INFO_BYTES_TO_READ) loc = buffer;
    } else if (bytes && length >= offset + IMAGE_INFO_BYTES_TO_READ) {
        loc = (const char*)bytes + offset;
    }
    if (loc) {
        if (sixtyFour) {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)loc)->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header_64 *)loc)->sizeofcmds, swapped);
            const char *startofcmds = loc + sizeof(struct mach_header_64);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct segment_command_64 *sgp = (struct segment_command_64 *)startofcmds;
            if (endofcmds > loc + IMAGE_INFO_BYTES_TO_READ) endofcmds = loc + IMAGE_INFO_BYTES_TO_READ;
            for (i = 0; !foundit && i < ncmds && startofcmds <= (char *)sgp && (char *)sgp < endofcmds; i++) {
                if (LC_SEGMENT_64 == _CFBundleSwapInt32Conditional(sgp->cmd, swapped)) {
                    struct section_64 *sp = (struct section_64 *)((char *)sgp + sizeof(struct segment_command_64));
                    uint32_t nsects = _CFBundleSwapInt32Conditional(sgp->nsects, swapped);
                    for (j = 0; !foundit && j < nsects && startofcmds <= (char *)sp && (char *)sp < endofcmds; j++) {
                        if (0 == strncmp(sp->segname, OBJC_SEGMENT, sizeof(sp->segname))) localHasObjc = true;
                        if (0 == strncmp(sp->sectname, IMAGE_INFO_SECTION, sizeof(sp->sectname)) && 0 == strncmp(sp->segname, OBJC_SEGMENT, sizeof(sp->segname))) {
                            uint64_t sectlength64 = _CFBundleSwapInt64Conditional(sp->size, swapped);
                            sectlength = (uint32_t)(sectlength64 & 0xffffffff);
                            sectoffset = _CFBundleSwapInt32Conditional(sp->offset, swapped);
                            foundit = true;
                        }
                        sp = (struct section_64 *)((char *)sp + sizeof(struct section_64));
                    }
                }
                sgp = (struct segment_command_64 *)((char *)sgp + _CFBundleSwapInt32Conditional(sgp->cmdsize, swapped));
            }
        } else {
            uint32_t ncmds = _CFBundleSwapInt32Conditional(((struct mach_header *)loc)->ncmds, swapped);
            uint32_t sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header *)loc)->sizeofcmds, swapped);
            const char *startofcmds = loc + sizeof(struct mach_header);
            const char *endofcmds = startofcmds + sizeofcmds;
            struct segment_command *sgp = (struct segment_command *)startofcmds;
            if (endofcmds > loc + IMAGE_INFO_BYTES_TO_READ) endofcmds = loc + IMAGE_INFO_BYTES_TO_READ;
            for (i = 0; !foundit && i < ncmds && startofcmds <= (char *)sgp && (char *)sgp < endofcmds; i++) {
                if (LC_SEGMENT == _CFBundleSwapInt32Conditional(sgp->cmd, swapped)) {
                    struct section *sp = (struct section *)((char *)sgp + sizeof(struct segment_command));
                    uint32_t nsects = _CFBundleSwapInt32Conditional(sgp->nsects, swapped);
                    for (j = 0; !foundit && j < nsects && startofcmds <= (char *)sp && (char *)sp < endofcmds; j++) {
                        if (0 == strncmp(sp->segname, OBJC_SEGMENT, sizeof(sp->segname))) localHasObjc = true;
                        if (0 == strncmp(sp->sectname, IMAGE_INFO_SECTION, sizeof(sp->sectname)) && 0 == strncmp(sp->segname, OBJC_SEGMENT, sizeof(sp->segname))) {
                            sectlength = _CFBundleSwapInt32Conditional(sp->size, swapped);
                            sectoffset = _CFBundleSwapInt32Conditional(sp->offset, swapped);
                            foundit = true;
                        }
                        sp = (struct section *)((char *)sp + sizeof(struct section));
                    }
                }
                sgp = (struct segment_command *)((char *)sgp + _CFBundleSwapInt32Conditional(sgp->cmdsize, swapped));
            }
        }
        if (sectlength >= 8) {
            if (fd >= 0 && lseek(fd, offset + sectoffset, SEEK_SET) == (off_t)(offset + sectoffset) && read(fd, sectbuffer, 8) >= 8) {
                localVersion = _CFBundleSwapInt32Conditional(*(uint32_t *)sectbuffer, swapped);
                localFlags = _CFBundleSwapInt32Conditional(*(uint32_t *)(sectbuffer + 4), swapped);
            } else if (bytes && length >= offset + sectoffset + 8) {
                localVersion = _CFBundleSwapInt32Conditional(*(uint32_t *)((uint32_t*)bytes + offset + sectoffset), swapped);
                localFlags = _CFBundleSwapInt32Conditional(*(uint32_t *)((uint32_t*)bytes + offset + sectoffset + 4), swapped);
            }
        }
    }
    
    if (buffer) free(buffer);
    
    if (hasObjc) *hasObjc = localHasObjc;
    if (objcVersion) *objcVersion = localVersion;
    if (objcFlags) *objcFlags = localFlags;
}
    
static UInt32 _CFBundleGrokMachTypeForFatFile(int fd, const void *bytes, CFIndex length, Boolean *isX11, CFArrayRef *architectures, CFDictionaryRef *infodict, Boolean *hasObjc, uint32_t *objcVersion, uint32_t *objcFlags) {
    UInt32 machtype = UNKNOWN_FILETYPE, magic, numFatHeaders = ((struct fat_header *)bytes)->nfat_arch, maxFatHeaders = (length - sizeof(struct fat_header)) / sizeof(struct fat_arch), i;
    unsigned char buffer[sizeof(struct mach_header_64)];
    const unsigned char *moreBytes = NULL;
    const NXArchInfo *archInfo = NXGetLocalArchInfo();
    struct fat_arch *fat = NULL;

    if (isX11) *isX11 = false;
    if (architectures) *architectures = NULL;
    if (infodict) *infodict = NULL;
    if (hasObjc) *hasObjc = false;
    if (objcVersion) *objcVersion = 0;
    if (objcFlags) *objcFlags = 0;
    if (numFatHeaders > maxFatHeaders) numFatHeaders = maxFatHeaders;
    if (numFatHeaders > 0) {
        fat = NXFindBestFatArch(archInfo->cputype, archInfo->cpusubtype, (struct fat_arch *)((struct fat_arch*)bytes + sizeof(struct fat_header)), numFatHeaders);
        if (!fat) fat = (struct fat_arch *)((struct fat_arch *)bytes + sizeof(struct fat_header));
        if (architectures) {
            CFMutableArrayRef mutableArchitectures = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            for (i = 0; i < numFatHeaders; i++) {
                CFNumberRef architecture = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt32Type, (struct fat_header *)bytes + sizeof(struct fat_header) + i * sizeof(struct fat_arch));
                if (CFArrayGetFirstIndexOfValue(mutableArchitectures, CFRangeMake(0, CFArrayGetCount(mutableArchitectures)), architecture) < 0) CFArrayAppendValue(mutableArchitectures, architecture);
                CFRelease(architecture);
            }
            *architectures = (CFArrayRef)mutableArchitectures;
        }
    } 
    if (fat) {
        if (fd >= 0 && lseek(fd, fat->offset, SEEK_SET) == (off_t)fat->offset && read(fd, buffer, sizeof(struct mach_header_64)) >= (int)sizeof(struct mach_header_64)) {
            moreBytes = buffer;
        } else if (bytes && (uint32_t)length >= fat->offset + sizeof(struct mach_header_64)) {
            moreBytes = (const unsigned char *)bytes + fat->offset;
        }
        if (moreBytes) {
            magic = *((UInt32 *)moreBytes);
            if (MH_MAGIC == magic) {
                machtype = ((struct mach_header *)moreBytes)->filetype;
                if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, fat->offset, false, false);
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, fat->offset, false, false);
                if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, fat->offset, false, false, hasObjc, objcVersion, objcFlags);
            } else if (MH_CIGAM == magic) {
                machtype = CFSwapInt32(((struct mach_header *)moreBytes)->filetype);
                if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, fat->offset, true, false);
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, fat->offset, true, false);
                if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, fat->offset, true, false, hasObjc, objcVersion, objcFlags);
            } else if (MH_MAGIC_64 == magic) {
                machtype = ((struct mach_header_64 *)moreBytes)->filetype;
                if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, fat->offset, false, true);
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, fat->offset, false, true);
                if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, fat->offset, false, true, hasObjc, objcVersion, objcFlags);
            } else if (MH_CIGAM_64 == magic) {
                machtype = CFSwapInt32(((struct mach_header_64 *)moreBytes)->filetype);
                if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, fat->offset, true, true);
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, fat->offset, true, true);
                if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, fat->offset, true, true, hasObjc, objcVersion, objcFlags);
            }
        }
    }
    return machtype;
}

static UInt32 _CFBundleGrokMachType(int fd, const void *bytes, CFIndex length, Boolean *isX11, CFArrayRef *architectures, CFDictionaryRef *infodict, Boolean *hasObjc, uint32_t *objcVersion, uint32_t *objcFlags) {
    unsigned int magic = *((UInt32 *)bytes), machtype = UNKNOWN_FILETYPE;
    CFNumberRef architecture = NULL;
    CFIndex i;

    if (isX11) *isX11 = false;
    if (architectures) *architectures = NULL;
    if (infodict) *infodict = NULL;
    if (hasObjc) *hasObjc = false;
    if (objcVersion) *objcVersion = 0;
    if (objcFlags) *objcFlags = 0;
    if (MH_MAGIC == magic) {
        machtype = ((struct mach_header *)bytes)->filetype;
        if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, 0, false, false);
        if (architectures) architecture = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt32Type, (char*)bytes + 4);
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, 0, false, false);
        if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, 0, false, false, hasObjc, objcVersion, objcFlags);
    } else if (MH_CIGAM == magic) {
        for (i = 0; i < length; i += 4) *(UInt32 *)((char*)bytes + i) = CFSwapInt32(*(UInt32 *)((char*)bytes + i));
        machtype = ((struct mach_header *)bytes)->filetype;
        if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, 0, true, false);
        if (architectures) architecture = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt32Type, (char*)bytes + 4);
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, 0, true, false);
        if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, 0, true, false, hasObjc, objcVersion, objcFlags);
    } else if (MH_MAGIC_64 == magic) {
        machtype = ((struct mach_header_64 *)bytes)->filetype;
        if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, 0, false, true);
        if (architectures) architecture = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt32Type, (char*)bytes + 4);
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, 0, false, true);
        if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, 0, false, true, hasObjc, objcVersion, objcFlags);
    } else if (MH_CIGAM_64 == magic) {
        for (i = 0; i < length; i += 4) *(UInt32 *)((char*)bytes + i) = CFSwapInt32(*(UInt32 *)((char*)bytes + i));
        machtype = ((struct mach_header_64 *)bytes)->filetype;
        if (isX11 && MH_EXECUTE == machtype) *isX11 = _CFBundleGrokX11FromFile(fd, bytes, length, 0, true, true);
        if (architectures) architecture = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt32Type, (char*)bytes + 4);
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, bytes, length, 0, true, true);
        if (hasObjc || objcVersion || objcFlags) _CFBundleGrokObjcImageInfoFromFile(fd, bytes, length, 0, true, true, hasObjc, objcVersion, objcFlags);
    } else if (FAT_MAGIC == magic) {
        machtype = _CFBundleGrokMachTypeForFatFile(fd, bytes, length, isX11, architectures, infodict, hasObjc, objcVersion, objcFlags);
    } else if (FAT_CIGAM == magic) {
        for (i = 0; i < length; i += 4) *(UInt32 *)((char*)bytes + i) = CFSwapInt32(*(UInt32 *)((char*)bytes + i));
        machtype = _CFBundleGrokMachTypeForFatFile(fd, bytes, length, isX11, architectures, infodict, hasObjc, objcVersion, objcFlags);
    } else if (PEF_MAGIC == magic || PEF_CIGAM == magic) {
        machtype = PEF_FILETYPE;
    }
    if (architectures && architecture) *architectures = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&architecture, 1, &kCFTypeArrayCallBacks);
    if (architecture) CFRelease(architecture);
    return machtype;
}

#endif /* BINARY_SUPPORT_DYLD */

static Boolean _CFBundleGrokFileTypeForZipMimeType(const unsigned char *bytes, CFIndex length, const char **ext) {
    unsigned namelength = CFSwapInt16HostToLittle(*((UInt16 *)(bytes + 26))), extralength = CFSwapInt16HostToLittle(*((UInt16 *)(bytes + 28)));
    const unsigned char *data = bytes + 30 + namelength + extralength;
    int i = -1;
    if (bytes < data && data + 56 <= bytes + length && 0 == CFSwapInt16HostToLittle(*((UInt16 *)(bytes + 8))) && (0 == ustrncasecmp(data, "application/vnd.", 16) || 0 == ustrncasecmp(data, "application/x-vnd.", 18))) {
        data += ('.' == *(data + 15)) ? 16 : 18;
        if (0 == ustrncasecmp(data, "sun.xml.", 8)) {
            data += 8;
            if (0 == ustrncasecmp(data, "calc", 4)) i = 0;
            else if (0 == ustrncasecmp(data, "draw", 4)) i = 1;
            else if (0 == ustrncasecmp(data, "writer.global", 13)) i = 2;
            else if (0 == ustrncasecmp(data, "impress", 7)) i = 3;
            else if (0 == ustrncasecmp(data, "math", 4)) i = 4;
            else if (0 == ustrncasecmp(data, "writer", 6)) i = 5;
            if (i >= 0 && ext) *ext = __CFBundleOOExtensionsArray + i * EXTENSION_LENGTH;
        } else if (0 == ustrncasecmp(data, "oasis.opendocument.", 19)) {
            data += 19;
            if (0 == ustrncasecmp(data, "chart", 5)) i = 0;
            else if (0 == ustrncasecmp(data, "formula", 7)) i = 1;
            else if (0 == ustrncasecmp(data, "graphics", 8)) i = 2;
            else if (0 == ustrncasecmp(data, "text-web", 8)) i = 3;
            else if (0 == ustrncasecmp(data, "image", 5)) i = 4;
            else if (0 == ustrncasecmp(data, "text-master", 11)) i = 5;
            else if (0 == ustrncasecmp(data, "presentation", 12)) i = 6;
            else if (0 == ustrncasecmp(data, "spreadsheet", 11)) i = 7;
            else if (0 == ustrncasecmp(data, "text", 4)) i = 8;
            if (i >= 0 && ext) *ext = __CFBundleODExtensionsArray + i * EXTENSION_LENGTH;
        }
    } else if (bytes < data && data + 41 <= bytes + length && 8 == CFSwapInt16HostToLittle(*((UInt16 *)(bytes + 8))) && 0x4b2c28c8 == CFSwapInt32HostToBig(*((UInt32 *)data)) && 0xc94c4e2c == CFSwapInt32HostToBig(*((UInt32 *)(data + 4)))) {
        // AbiWord compressed mimetype odt
        if (ext) *ext = "odt";
    }
    return (i >= 0);
}

static const char *_CFBundleGrokFileTypeForZipFile(int fd, const unsigned char *bytes, CFIndex length, off_t fileLength) {
    const char *ext = "zip";
    const unsigned char *moreBytes = NULL;
    unsigned char *buffer = NULL;
    CFIndex i;
    Boolean foundMimetype = false, hasMetaInf = false, hasContentXML = false, hasManifestMF = false, hasManifestXML = false, hasRels = false, hasContentTypes = false, hasWordDocument = false, hasExcelDocument = false, hasPowerPointDocument = false, hasOPF = false, hasSMIL = false;

    if (bytes) {
        for (i = 0; !foundMimetype && i + 30 < length; i++) {
            if (0x50 == bytes[i] && 0x4b == bytes[i + 1]) {
                unsigned namelength = 0, offset = 0;
                if (0x01 == bytes[i + 2] && 0x02 == bytes[i + 3]) {
                    namelength = (unsigned)CFSwapInt16HostToLittle(*((UInt16 *)(bytes + i + 28)));
                    offset = 46;
                } else if (0x03 == bytes[i + 2] && 0x04 == bytes[i + 3]) {
                    namelength = (unsigned)CFSwapInt16HostToLittle(*((UInt16 *)(bytes + i + 26)));
                    offset = 30;
                }
                if (offset > 0 && (CFIndex)(i + offset + namelength) <= length) {
                    //printf("%.*s\n", namelength, bytes + i + offset);
                    if (8 == namelength && 30 == offset && 0 == ustrncasecmp(bytes + i + offset, "mimetype", 8)) foundMimetype = _CFBundleGrokFileTypeForZipMimeType(bytes + i, length - i, &ext);
                    else if (9 == namelength && 0 == ustrncasecmp(bytes + i + offset, "META-INF/", 9)) hasMetaInf = true;
                    else if (11 == namelength && 0 == ustrncasecmp(bytes + i + offset, "content.xml", 11)) hasContentXML = true;
                    else if (11 == namelength && 0 == ustrncasecmp(bytes + i + offset, "_rels/.rels", 11)) hasRels = true;
                    else if (19 == namelength && 0 == ustrncasecmp(bytes + i + offset, "[Content_Types].xml", 19)) hasContentTypes = true;
                    else if (20 == namelength && 0 == ustrncasecmp(bytes + i + offset, "META-INF/MANIFEST.MF", 20)) hasManifestMF = true;
                    else if (21 == namelength && 0 == ustrncasecmp(bytes + i + offset, "META-INF/manifest.xml", 21)) hasManifestXML = true;
                    else if (4 < namelength && 0 == ustrncasecmp(bytes + i + offset + namelength - 4, ".opf", 4)) hasOPF = true;
                    else if (4 < namelength && 0 == ustrncasecmp(bytes + i + offset + namelength - 4, ".sml", 4)) hasSMIL = true;
                    else if (5 < namelength && 0 == ustrncasecmp(bytes + i + offset + namelength - 5, ".smil", 5)) hasSMIL = true;
                    else if (9 < namelength && 0 == ustrncasecmp(bytes + i + offset, "word/", 5) && 0 == ustrncasecmp(bytes + i + offset + namelength - 4, ".xml", 4)) hasWordDocument = true;
                    else if (10 < namelength && 0 == ustrncasecmp(bytes + i + offset, "excel/", 6) && 0 == ustrncasecmp(bytes + i + offset + namelength - 4, ".xml", 4)) hasExcelDocument = true;
                    else if (15 < namelength && 0 == ustrncasecmp(bytes + i + offset, "powerpoint/", 11) && 0 == ustrncasecmp(bytes + i + offset + namelength - 4, ".xml", 4)) hasPowerPointDocument = true;
                    i += offset + namelength - 1;
                }
            }
        }
    }
    if (!foundMimetype) {
        if (fileLength >= ZIP_BYTES_TO_READ) {
            if (fd >= 0 && lseek(fd, fileLength - ZIP_BYTES_TO_READ, SEEK_SET) == fileLength - ZIP_BYTES_TO_READ) {
                buffer = (unsigned char *)malloc(ZIP_BYTES_TO_READ);
                if (buffer && read(fd, buffer, ZIP_BYTES_TO_READ) >= ZIP_BYTES_TO_READ) moreBytes = buffer;
            } else if (bytes && length >= ZIP_BYTES_TO_READ) {
                moreBytes = bytes + length - ZIP_BYTES_TO_READ;
            }
        }
        if (moreBytes) {
            for (i = 0; i + 30 < ZIP_BYTES_TO_READ; i++) {
                if (0x50 == moreBytes[i] && 0x4b == moreBytes[i + 1]) {
                    unsigned namelength = 0, offset = 0;
                    if (0x01 == moreBytes[i + 2] && 0x02 == moreBytes[i + 3]) {
                        namelength = CFSwapInt16HostToLittle(*((UInt16 *)(moreBytes + i + 28)));
                        offset = 46;
                    } else if (0x03 == moreBytes[i + 2] && 0x04 == moreBytes[i + 3]) {
                        namelength = CFSwapInt16HostToLittle(*((UInt16 *)(moreBytes + i + 26)));
                        offset = 30;
                    }
                    if (offset > 0 && i + offset + namelength <= ZIP_BYTES_TO_READ) {
                        //printf("%.*s\n", namelength, moreBytes + i + offset);
                        if (9 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "META-INF/", 9)) hasMetaInf = true;
                        else if (11 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "content.xml", 11)) hasContentXML = true;
                        else if (11 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "_rels/.rels", 11)) hasRels = true;
                        else if (19 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "[Content_Types].xml", 19)) hasContentTypes = true;
                        else if (20 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "META-INF/MANIFEST.MF", 20)) hasManifestMF = true;
                        else if (21 == namelength && 0 == ustrncasecmp(moreBytes + i + offset, "META-INF/manifest.xml", 21)) hasManifestXML = true;
                        else if (4 < namelength && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 4, ".opf", 4)) hasOPF = true;
                        else if (4 < namelength && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 4, ".sml", 4)) hasSMIL = true;
                        else if (5 < namelength && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 5, ".smil", 5)) hasSMIL = true;
                        else if (9 < namelength && 0 == ustrncasecmp(moreBytes + i + offset, "word/", 5) && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 4, ".xml", 4)) hasWordDocument = true;
                        else if (10 < namelength && 0 == ustrncasecmp(moreBytes + i + offset, "excel/", 6) && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 4, ".xml", 4)) hasExcelDocument = true;
                        else if (15 < namelength && 0 == ustrncasecmp(moreBytes + i + offset, "powerpoint/", 11) && 0 == ustrncasecmp(moreBytes + i + offset + namelength - 4, ".xml", 4)) hasPowerPointDocument = true;
                        i += offset + namelength - 1;
                    }
                }
            }
        }
        //printf("hasManifestMF %d hasManifestXML %d hasContentXML %d hasRels %d hasContentTypes %d hasWordDocument %d hasExcelDocument %d hasPowerPointDocument %d hasMetaInf %d hasOPF %d hasSMIL %d\n", hasManifestMF, hasManifestXML, hasContentXML, hasRels, hasContentTypes, hasWordDocument, hasExcelDocument, hasPowerPointDocument, hasMetaInf, hasOPF, hasSMIL);
        if (hasManifestMF) ext = "jar";
        else if ((hasRels || hasContentTypes) && hasWordDocument) ext = "docx";
        else if ((hasRels || hasContentTypes) && hasExcelDocument) ext = "xlsx";
        else if ((hasRels || hasContentTypes) && hasPowerPointDocument) ext = "pptx";
        else if (hasManifestXML || hasContentXML) ext = "odt";
        else if (hasMetaInf) ext = "jar";
        else if (hasOPF && hasSMIL) ext = "dtb";
        else if (hasOPF) ext = "oeb";

        if (buffer) free(buffer);
    }
    return ext;
}

static Boolean _CFBundleCheckOLEName(const char *name, const char *bytes, unsigned length) {
    Boolean retval = true;
    unsigned j;
    for (j = 0; retval && j < length; j++) if (bytes[2 * j] != name[j]) retval = false;
    return retval;
}

static const char *_CFBundleGrokFileTypeForOLEFile(int fd, const void *bytes, CFIndex length, off_t offset) {
    const char *ext = "ole", *moreBytes = NULL;
    char *buffer = NULL;
    
    if (fd >= 0 && lseek(fd, offset, SEEK_SET) == (off_t)offset) {
        buffer = (char *)malloc(OLE_BYTES_TO_READ);
        if (buffer && read(fd, buffer, OLE_BYTES_TO_READ) >= OLE_BYTES_TO_READ) moreBytes = buffer;
    } else if (bytes && length >= offset + OLE_BYTES_TO_READ) {
        moreBytes = (char *)bytes + offset;
    }
    if (moreBytes) {
        Boolean foundit = false;
        unsigned i;
        for (i = 0; !foundit && i < 4; i++) {
            char namelength = moreBytes[128 * i + 64] / 2;
            foundit = true;
            if (sizeof(XLS_NAME) == namelength && _CFBundleCheckOLEName(XLS_NAME, moreBytes + 128 * i, namelength - 1)) ext = "xls";
            else if (sizeof(XLS_NAME2) == namelength && _CFBundleCheckOLEName(XLS_NAME2, moreBytes + 128 * i, namelength - 1)) ext = "xls";
            else if (sizeof(DOC_NAME) == namelength && _CFBundleCheckOLEName(DOC_NAME, moreBytes + 128 * i, namelength - 1)) ext = "doc";
            else if (sizeof(PPT_NAME) == namelength && _CFBundleCheckOLEName(PPT_NAME, moreBytes + 128 * i, namelength - 1)) ext = "ppt";
            else foundit = false;
        }
    }

    if (buffer) free(buffer);

    return ext;
}

static Boolean _CFBundleGrokFileType(CFURLRef url, CFDataRef data, CFStringRef *extension, UInt32 *machtype, CFArrayRef *architectures, CFDictionaryRef *infodict, Boolean *hasObjc, uint32_t *objcVersion, uint32_t *objcFlags) {
    struct stat statBuf;
    int fd = -1;
    char path[CFMaxPathSize];
    const unsigned char *bytes = NULL;
    unsigned char buffer[MAGIC_BYTES_TO_READ];
    CFIndex i, length = 0;
    off_t fileLength = 0;
    const char *ext = NULL;
    UInt32 mt = UNKNOWN_FILETYPE;
#if defined(BINARY_SUPPORT_DYLD)
    Boolean isX11 = false;
#endif /* BINARY_SUPPORT_DYLD */
    Boolean isFile = false, isPlain = true, isZero = true, isHTML = false;
    // extensions returned:  o, tool, x11app, pef, core, dylib, bundle, elf, jpeg, jp2, tiff, gif, png, pict, icns, ico, rtf, rtfd, pdf, ra, rm, au, aiff, aifc, wav, avi, wmv, ogg, flac, psd, mpeg, mid, zip, jar, sit, cpio, html, ps, mov, qtif, ttf, otf, sfont, bmp, hqx, bin, class, tar, txt, gz, Z, uu, ync, bz, bz2, sh, pl, py, rb, dvi, sgi, tga, mp3, xml, plist, xls, doc, ppt, mp4, m4a, m4b, m4p, dmg, cwk, webarchive, dwg, dgn, pfa, pfb, afm, tfm, xcf, cpx, dwf, swf, swc, abw, bom, lit, svg, rdf, x3d, oeb, dtb, docx, xlsx, pptx, sxc, sxd, sxg, sxi, sxm, sxw, odc, odf, odg, oth, odi, odm, odp, ods
    // ??? we do not distinguish between different wm types, returning wmv for any of wmv, wma, or asf
    // ??? we do not distinguish between ordinary documents and template versions (often there is no difference in file contents)
    // ??? the distinctions between docx, xlsx, and pptx may not be entirely reliable
    if (architectures) *architectures = NULL;
    if (infodict) *infodict = NULL;
    if (hasObjc) *hasObjc = false;
    if (objcVersion) *objcVersion = 0;
    if (objcFlags) *objcFlags = 0;
    if (url && CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathSize) && stat(path, &statBuf) == 0 && (statBuf.st_mode & S_IFMT) == S_IFREG && (fd = open(path, O_RDONLY, 0777)) >= 0) {
        length = read(fd, buffer, MAGIC_BYTES_TO_READ);
        fileLength = statBuf.st_size;
        bytes = buffer;
        isFile = true;
    } else if (data) {
        length = CFDataGetLength(data);
        fileLength = (off_t)length;
        bytes = CFDataGetBytePtr(data);
        if (length == 0) ext = "txt";
    }
    if (bytes) {
        if (length >= 4) {
            UInt32 magic = CFSwapInt32HostToBig(*((UInt32 *)bytes));
            for (i = 0; !ext && i < NUM_EXTENSIONS; i++) {
                if (__CFBundleMagicNumbersArray[i] == magic) ext = __CFBundleExtensionsArray + i * EXTENSION_LENGTH;
            }
            if (ext) {
                if (0xcafebabe == magic && 8 <= length && 0 != *((UInt16 *)(bytes + 4))) ext = "class";
#if defined(BINARY_SUPPORT_DYLD)
                else if ((int)sizeof(struct mach_header_64) <= length) mt = _CFBundleGrokMachType(fd, bytes, length, extension ? &isX11 : NULL, architectures, infodict, hasObjc, objcVersion, objcFlags);
                
                if (MH_OBJECT == mt) ext = "o";
                else if (MH_EXECUTE == mt) ext = isX11 ? "x11app" : "tool";
                else if (PEF_FILETYPE == mt) ext = "pef";
                else if (MH_CORE == mt) ext = "core";
                else if (MH_DYLIB == mt) ext = "dylib";
                else if (MH_BUNDLE == mt) ext = "bundle";
#endif /* BINARY_SUPPORT_DYLD */
                else if (0x7b5c7274 == magic && (6 > length || 'f' != bytes[4])) ext = NULL;
                else if (0x00010000 == magic && (6 > length || 0 != bytes[4])) ext = NULL;
                else if (0x47494638 == magic && (6 > length || (0x3761 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))) && 0x3961 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))))))  ext = NULL;
                else if (0x0000000c == magic && (6 > length || 0x6a50 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))))) ext = NULL;
                else if (0x2356524d == magic && (6 > length || 0x4c20 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))))) ext = NULL;
                else if (0x28445746 == magic && (6 > length || 0x2056 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))))) ext = NULL;
                else if (0x30373037 == magic && (6 > length || 0x30 != bytes[4] || !isdigit(bytes[5]))) ext = NULL;
                else if (0x41433130 == magic && (6 > length || 0x31 != bytes[4] || !isdigit(bytes[5]))) ext = NULL;
                else if (0x89504e47 == magic && (8 > length || 0x0d0a1a0a != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x53747566 == magic && (8 > length || 0x66497420 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x3026b275 == magic && (8 > length || 0x8e66cf11 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x67696d70 == magic && (8 > length || 0x20786366 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x424f4d53 == magic && (8 > length || 0x746f7265 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x49544f4c == magic && (8 > length || 0x49544c53 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x72746664 == magic && (8 > length || 0x00000000 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = NULL;
                else if (0x3d796265 == magic && (12 > length || 0x67696e20 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))) || (0x6c696e65 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8))) && 0x70617274 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))))) ext = NULL;
                else if (0x25215053 == magic && 14 <= length && 0 == ustrncmp(bytes + 4, "-AdobeFont", 10)) ext = "pfa"; 
                else if (0x504b0304 == magic) ext = _CFBundleGrokFileTypeForZipFile(fd, bytes, length, fileLength);
                else if (0x464f524d == magic) {
                    // IFF
                    ext = NULL;
                    if (12 <= length) {
                        UInt32 iffMagic = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)));
                        if (0x41494646 == iffMagic) ext = "aiff";
                        else if (0x414946 == iffMagic) ext = "aifc";
                    }
                } else if (0x52494646 == magic) {
                    // RIFF
                    ext = NULL;
                    if (12 <= length) {
                        UInt32 riffMagic = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)));
                        if (0x57415645 == riffMagic) ext = "wav";
                        else if (0x41564920 == riffMagic) ext = "avi";
                    }
                } else if (0xd0cf11e0 == magic) {
                    // OLE
                    if (52 <= length) ext = _CFBundleGrokFileTypeForOLEFile(fd, bytes, length, 512 * (1 + CFSwapInt32HostToLittle(*((UInt32 *)(bytes + 48)))));
                } else if (0x62656769 == magic) {
                    // uu
                    ext = NULL;
                    if (76 <= length && 'n' == bytes[4] && ' ' == bytes[5] && isdigit(bytes[6]) && isdigit(bytes[7]) && isdigit(bytes[8]) && ' ' == bytes[9]) {
                        CFIndex endOfLine = 0;
                        for (i = 10; 0 == endOfLine && i < length; i++) if ('\n' == bytes[i]) endOfLine = i;
                        if (10 <= endOfLine && endOfLine + 62 < length && 'M' == bytes[endOfLine + 1] && '\n' == bytes[endOfLine + 62]) {
                            ext = "uu";
                            for (i = endOfLine + 1; ext && i < endOfLine + 62; i++) if (!isprint(bytes[i])) ext = NULL;
                        }
                    }
                }
            }
            if (extension && !ext) {
                UInt16 shortMagic = CFSwapInt16HostToBig(*((UInt16 *)bytes));
                if (5 <= length && 0 == bytes[3] && 0 == bytes[4] && ((1 == bytes[1] && 1 == (0xf7 & bytes[2])) || (0 == bytes[1] && (2 == (0xf7 & bytes[2]) || (3 == (0xf7 & bytes[2])))))) ext = "tga";
                else if (8 <= length && (0x6d6f6f76 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))) || 0x6d646174 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))) || 0x77696465 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = "mov";
                else if (8 <= length && (0x69647363 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))) || 0x69646174 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = "qtif";
                else if (8 <= length && 0x424f424f == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4)))) ext = "cwk";
                else if (8 <= length && 0x62706c69 == magic && 0x7374 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))) && isdigit(bytes[6]) && isdigit(bytes[7])) {
                    for (i = 8; !ext && i < 128 && i + 16 <= length; i++) {
                        if (0 == ustrncmp(bytes + i, "WebMainResource", 15)) ext = "webarchive";
                    }
                    if (!ext) ext = "plist";
                } else if (12 <= length && 0x66747970 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4)))) {
                    // ??? list of ftyp values needs to be checked
                    if (0x6d703432 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) ext = "mp4";
                    else if (0x4d344120 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) ext = "m4a";
                    else if (0x4d344220 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) ext = "m4b";
                    else if (0x4d345020 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) ext = "m4p";
                } else if (0x424d == shortMagic && 18 <= length && 40 == CFSwapInt32HostToLittle(*((UInt32 *)(bytes + 14)))) ext = "bmp";
                else if (20 <= length && 0 == ustrncmp(bytes + 6, "%!PS-AdobeFont", 14)) ext = "pfb";
                else if (40 <= length && 0x42696e48 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 34))) && 0x6578 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 38)))) ext = "hqx";
                else if (128 <= length && 0x6d42494e == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 102)))) ext = "bin";
                else if (128 <= length && 0 == bytes[0] && 0 < bytes[1] && bytes[1] < 64 && 0 == bytes[74] && 0 == bytes[82] && 0 == (fileLength % 128)) {
                    unsigned df = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 83))), rf = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 87))), blocks = 1 + (df + 127) / 128 + (rf + 127) / 128;
                    if (df < 0x00800000 && rf < 0x00800000 && 1 < blocks && (off_t)(128 * blocks) == fileLength) ext = "bin";
                } else if (265 <= length && 0x75737461 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 257))) && (0x72202000 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 261))) || 0x7200 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 261))))) ext = "tar";
                else if (0xfeff == shortMagic || 0xfffe == shortMagic) ext = "txt";
                else if (0x1f9d == shortMagic) ext = "Z";
                else if (0x1f8b == shortMagic) ext = "gz";
                else if (0x71c7 == shortMagic || 0xc771 == shortMagic) ext = "cpio";
                else if (0xf702 == shortMagic) ext = "dvi";
                else if (0x01da == shortMagic && (0 == bytes[2] || 1 == bytes[2]) && (0 < bytes[3] && 16 > bytes[3])) ext = "sgi";
                else if (0x2321 == shortMagic) {
                    CFIndex endOfLine = 0, lastSlash = 0;
                    for (i = 2; 0 == endOfLine && i < length; i++) if ('\n' == bytes[i]) endOfLine = i;
                    if (endOfLine > 3) {
                        for (i = endOfLine - 1; 0 == lastSlash && i > 1; i--) if ('/' == bytes[i]) lastSlash = i;
                        if (lastSlash > 0) {
                            if (0 == ustrncmp(bytes + lastSlash + 1, "perl", 4)) ext = "pl";
                            else if (0 == ustrncmp(bytes + lastSlash + 1, "python", 6)) ext = "py";
                            else if (0 == ustrncmp(bytes + lastSlash + 1, "ruby", 4)) ext = "rb";
                            else ext = "sh";
                        }
                    } 
                } else if (0xffd8 == shortMagic && 0xff == bytes[2]) ext = "jpeg";
                else if (0x4657 == shortMagic && 0x53 == bytes[2]) ext = "swf";
                else if (0x4357 == shortMagic && 0x53 == bytes[2]) ext = "swc";
                else if (0x4944 == shortMagic && '3' == bytes[2] && 0x20 > bytes[3]) ext = "mp3";
                else if (0x425a == shortMagic && isdigit(bytes[2]) && isdigit(bytes[3])) ext = "bz";
                else if (0x425a == shortMagic && 'h' == bytes[2] && isdigit(bytes[3]) && 8 <= length && (0x31415926 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))) || 0x17724538 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) ext = "bz2";
                else if (0x0011 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 2))) || 0x0012 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 2)))) ext = "tfm";
                else if ('<' == bytes[0] && 14 <= length) {
                    if (0 == ustrncasecmp(bytes + 1, "!doctype html", 13) || 0 == ustrncasecmp(bytes + 1, "head", 4) || 0 == ustrncasecmp(bytes + 1, "title", 5) || 0 == ustrncasecmp(bytes + 1, "html", 4)) {
                        ext = "html";
                    } else if (0 == ustrncasecmp(bytes + 1, "?xml", 4)) {
                        for (i = 4; !ext && i < 128 && i + 20 <= length; i++) {
                            if ('<' == bytes[i]) {
                                if (0 == ustrncasecmp(bytes + i + 1, "abiword", 7)) ext = "abw";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype svg", 12)) ext = "svg";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype rdf", 12)) ext = "rdf";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype x3d", 12)) ext = "x3d";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype html", 13)) ext = "html";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype plist", 14)) ext = "plist";
                                else if (0 == ustrncasecmp(bytes + i + 1, "!doctype posingfont", 19)) ext = "sfont";
                            }
                        }
                        if (!ext) ext = "xml";
                    }
                }
            }
        }
        if (extension && !ext) {
            //??? what about MacOSRoman?
            for (i = 0; (isPlain || isZero) && !isHTML && i < length && i < 512; i++) {
                char c = bytes[i];
                if (0x7f <= c || (0x20 > c && !isspace(c))) isPlain = false;
                if (0 != c) isZero = false;
                if (isPlain && '<' == c && i + 14 <= length && 0 == ustrncasecmp(bytes + i + 1, "!doctype html", 13)) isHTML = true;
            }
            if (isHTML) {
                ext = "html";
            } else if (isPlain) {
                if (16 <= length && 0 == ustrncmp(bytes, "StartFontMetrics", 16)) ext = "afm";
                else ext = "txt";
            } else if (isZero && length >= MAGIC_BYTES_TO_READ && fileLength >= 526) {
                if (isFile) {
                    if (lseek(fd, 512, SEEK_SET) == 512 && read(fd, buffer, MAGIC_BYTES_TO_READ) >= 14) {
                        if (0x001102ff == CFSwapInt32HostToBig(*((UInt32 *)(buffer + 10)))) ext = "pict";
                    }
                } else {
                    if (526 <= length && 0x001102ff == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 522)))) ext = "pict";
                }
            }
        }
        if (extension && (!ext || 0 == strcmp(ext, "bz2")) && length >= MAGIC_BYTES_TO_READ && fileLength >= DMG_BYTES_TO_READ) {
            if (isFile) {
                if (lseek(fd, fileLength - DMG_BYTES_TO_READ, SEEK_SET) == fileLength - DMG_BYTES_TO_READ && read(fd, buffer, DMG_BYTES_TO_READ) >= DMG_BYTES_TO_READ) {
                    if (0x6b6f6c79 == CFSwapInt32HostToBig(*((UInt32 *)buffer)) || (0x63647361 == CFSwapInt32HostToBig(*((UInt32 *)(buffer + DMG_BYTES_TO_READ - 8))) && 0x656e6372 == CFSwapInt32HostToBig(*((UInt32 *)(buffer + DMG_BYTES_TO_READ - 4))))) ext = "dmg";
                }
            } else {
                if (DMG_BYTES_TO_READ <= length && (0x6b6f6c79 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + length - DMG_BYTES_TO_READ))) || (0x63647361 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + length - 8))) && 0x656e6372 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + length - 4)))))) ext = "dmg";
            }
        }
    }
    if (extension) *extension = ext ? CFStringCreateWithCStringNoCopy(kCFAllocatorSystemDefault, ext, kCFStringEncodingUTF8, kCFAllocatorNull) : NULL;
    if (machtype) *machtype = mt;
    if (fd >= 0) close(fd);
    return (ext ? true : false);
}

CFStringRef _CFBundleCopyFileTypeForFileURL(CFURLRef url) {
    CFStringRef extension = NULL;
    (void)_CFBundleGrokFileType(url, NULL, &extension, NULL, NULL, NULL, NULL, NULL, NULL);
    return extension;
}

CFStringRef _CFBundleCopyFileTypeForFileData(CFDataRef data) {
    CFStringRef extension = NULL;
    (void)_CFBundleGrokFileType(NULL, data, &extension, NULL, NULL, NULL, NULL, NULL, NULL);
    return extension;
}

__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInExecutable(CFURLRef url) {
    CFDictionaryRef result = NULL;
    (void)_CFBundleGrokFileType(url, NULL, NULL, NULL, NULL, &result, NULL, NULL, NULL);
    return result;
}

__private_extern__ CFArrayRef _CFBundleCopyArchitecturesForExecutable(CFURLRef url) {
    CFArrayRef result = NULL;
    (void)_CFBundleGrokFileType(url, NULL, NULL, NULL, &result, NULL, NULL, NULL, NULL);
    return result;
}

#if defined(BINARY_SUPPORT_DYLD)

static Boolean _CFBundleGetObjCImageInfoForExecutable(CFURLRef url, uint32_t *objcVersion, uint32_t *objcFlags) {
    Boolean retval = false;
    (void)_CFBundleGrokFileType(url, NULL, NULL, NULL, NULL, NULL, &retval, objcVersion, objcFlags);
    return retval;
}

__private_extern__ __CFPBinaryType _CFBundleGrokBinaryType(CFURLRef executableURL) {
    // Attempt to grok the type of the binary by looking for DYLD magic numbers.  If one of the DYLD magic numbers is found, find out what type of Mach-o file it is.  Otherwise, look for the PEF magic numbers to see if it is CFM (if we understand CFM).
    __CFPBinaryType result = executableURL ? __CFBundleUnreadableBinary : __CFBundleNoBinary;
    UInt32 machtype = UNKNOWN_FILETYPE;
    if (_CFBundleGrokFileType(executableURL, NULL, NULL, &machtype, NULL, NULL, NULL, NULL, NULL)) {
        switch (machtype) {
            case MH_EXECUTE:
                result = __CFBundleDYLDExecutableBinary;
                break;
            case MH_BUNDLE:
                result = __CFBundleDYLDBundleBinary;
                break;
            case MH_DYLIB:
                result = __CFBundleDYLDFrameworkBinary;
                break;
#if defined(BINARY_SUPPORT_CFM)
            case PEF_FILETYPE:
                result = __CFBundleCFMBinary;
                break;
#endif /* BINARY_SUPPORT_CFM */
        }
    }
    return result;
}

#endif /* BINARY_SUPPORT_DYLD */

void _CFBundleSetCFMConnectionID(CFBundleRef bundle, void *connectionID) {
#if defined(BINARY_SUPPORT_CFM)
    if (bundle->_binaryType == __CFBundleUnknownBinary || bundle->_binaryType == __CFBundleUnreadableBinary) {
        bundle->_binaryType = __CFBundleCFMBinary;
    }
#endif /* BINARY_SUPPORT_CFM */
    bundle->_connectionCookie = connectionID;
    bundle->_isLoaded = true;
}

static CFStringRef _CFBundleCopyLastPathComponent(CFBundleRef bundle) {
    CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
    CFStringRef str = CFURLCopyFileSystemPath(bundleURL, kCFURLPOSIXPathStyle);
    UniChar buff[CFMaxPathSize];
    CFIndex buffLen = CFStringGetLength(str), startOfLastDir = 0;

    CFRelease(bundleURL);
    if (buffLen > CFMaxPathSize) buffLen = CFMaxPathSize;
    CFStringGetCharacters(str, CFRangeMake(0, buffLen), buff);
    CFRelease(str);
    if (buffLen > 0) startOfLastDir = _CFStartOfLastPathComponent(buff, buffLen);
    return CFStringCreateWithCharacters(kCFAllocatorSystemDefault, &(buff[startOfLastDir]), buffLen - startOfLastDir);
}

static CFErrorRef _CFBundleCreateErrorDebug(CFAllocatorRef allocator, CFBundleRef bundle, CFIndex code, CFStringRef debugString) {
    const void *userInfoKeys[6], *userInfoValues[6];
    CFIndex numKeys = 0;
    CFURLRef bundleURL = CFBundleCopyBundleURL(bundle), absoluteURL = CFURLCopyAbsoluteURL(bundleURL), executableURL = CFBundleCopyExecutableURL(bundle);
    CFBundleRef bdl = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.CoreFoundation"));
    CFStringRef bundlePath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE), executablePath = executableURL ? CFURLCopyFileSystemPath(executableURL, PLATFORM_PATH_STYLE) : NULL, descFormat = NULL, desc = NULL, reason = NULL, suggestion = NULL;
    CFErrorRef error;
    if (bdl) {
        CFStringRef name = (CFStringRef)CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleNameKey);
        name = name ? (CFStringRef)CFRetain(name) : _CFBundleCopyLastPathComponent(bundle);
        if (CFBundleExecutableNotFoundError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr4"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded because its executable could not be located."), "NSFileNoSuchFileError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr4-C"), CFSTR("Error"), bdl, CFSTR("The bundle\\U2019s executable could not be located."), "NSFileNoSuchFileError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr4-R"), CFSTR("Error"), bdl, CFSTR("Try reinstalling the bundle."), "NSFileNoSuchFileError");
        } else if (CFBundleExecutableNotLoadableError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3584"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded because its executable is not loadable."), "NSExecutableNotLoadableError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3584-C"), CFSTR("Error"), bdl, CFSTR("The bundle\\U2019s executable is not loadable."), "NSExecutableNotLoadableError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3584-R"), CFSTR("Error"), bdl, CFSTR("Try reinstalling the bundle."), "NSExecutableNotLoadableError");
        } else if (CFBundleExecutableArchitectureMismatchError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3585"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded because it does not contain a version for the current architecture."), "NSExecutableArchitectureMismatchError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3585-C"), CFSTR("Error"), bdl, CFSTR("The bundle does not contain a version for the current architecture."), "NSExecutableArchitectureMismatchError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3585-R"), CFSTR("Error"), bdl, CFSTR("Try installing a universal version of the bundle."), "NSExecutableArchitectureMismatchError");
        } else if (CFBundleExecutableRuntimeMismatchError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3586"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded because it is not compatible with the current application."), "NSExecutableRuntimeMismatchError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3586-C"), CFSTR("Error"), bdl, CFSTR("The bundle is not compatible with this application."), "NSExecutableRuntimeMismatchError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3586-R"), CFSTR("Error"), bdl, CFSTR("Try installing a newer version of the bundle."), "NSExecutableRuntimeMismatchError");
        } else if (CFBundleExecutableLoadError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3587"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded because it is damaged or missing necessary resources."), "NSExecutableLoadError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3587-C"), CFSTR("Error"), bdl, CFSTR("The bundle is damaged or missing necessary resources."), "NSExecutableLoadError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3587-R"), CFSTR("Error"), bdl, CFSTR("Try reinstalling the bundle."), "NSExecutableLoadError");
        } else if (CFBundleExecutableLinkError == code) {
            descFormat = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3588"), CFSTR("Error"), bdl, CFSTR("The bundle \\U201c%@\\U201d could not be loaded."), "NSExecutableLinkError");
            reason = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3588-C"), CFSTR("Error"), bdl, CFSTR("The bundle could not be loaded."), "NSExecutableLinkError");
            suggestion = CFCopyLocalizedStringWithDefaultValue(CFSTR("BundleErr3588-R"), CFSTR("Error"), bdl, CFSTR("Try reinstalling the bundle."), "NSExecutableLinkError");
        }
        if (descFormat) {
            desc = CFStringCreateWithFormat(allocator, NULL, descFormat, name);
            CFRelease(descFormat);
        }
        CFRelease(name);
    }
    if (bundlePath) {
        userInfoKeys[numKeys] = CFSTR("NSBundlePath");
        userInfoValues[numKeys] = bundlePath;
        numKeys++;
    }
    if (executablePath) {
        userInfoKeys[numKeys] = CFSTR("NSFilePath");
        userInfoValues[numKeys] = executablePath;
        numKeys++;
    }
    if (desc) {
        userInfoKeys[numKeys] = kCFErrorLocalizedDescriptionKey;
        userInfoValues[numKeys] = desc;
        numKeys++;
    }
    if (reason) {
        userInfoKeys[numKeys] = kCFErrorLocalizedFailureReasonKey;
        userInfoValues[numKeys] = reason;
        numKeys++;
    }
    if (suggestion) {
        userInfoKeys[numKeys] = kCFErrorLocalizedRecoverySuggestionKey;
        userInfoValues[numKeys] = suggestion;
        numKeys++;
    }
    if (debugString) {
        userInfoKeys[numKeys] = CFSTR("NSDebugDescription");
        userInfoValues[numKeys] = debugString;
        numKeys++;
    }
    error = CFErrorCreateWithUserInfoKeysAndValues(allocator, kCFErrorDomainCocoa, code, userInfoKeys, userInfoValues, numKeys);
    if (bundleURL) CFRelease(bundleURL);
    if (absoluteURL) CFRelease(absoluteURL);
    if (executableURL) CFRelease(executableURL);
    if (bundlePath) CFRelease(bundlePath);
    if (executablePath) CFRelease(executablePath);
    if (desc) CFRelease(desc);
    if (reason) CFRelease(reason);
    if (suggestion) CFRelease(suggestion);
    return error;
}

CFErrorRef _CFBundleCreateError(CFAllocatorRef allocator, CFBundleRef bundle, CFIndex code) {
    return _CFBundleCreateErrorDebug(allocator, bundle, code, NULL);
}

Boolean _CFBundleLoadExecutableAndReturnError(CFBundleRef bundle, Boolean forceGlobal, CFErrorRef *error) {
    Boolean result = false;
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

    if (!executableURL) bundle->_binaryType = __CFBundleNoBinary;
    // make sure we know whether bundle is already loaded or not
#if defined(BINARY_SUPPORT_DLFCN)
    if (!bundle->_isLoaded && _useDlfcn) _CFBundleDlfcnCheckLoaded(bundle);
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD)
    if (!bundle->_isLoaded) _CFBundleDYLDCheckLoaded(bundle);
    // We might need to figure out what it is
    if (bundle->_binaryType == __CFBundleUnknownBinary) {
        bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
        if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) bundle->_resourceData._executableLacksResourceFork = true;
#endif /* BINARY_SUPPORT_CFM */
    }
#endif /* BINARY_SUPPORT_DYLD */
    if (executableURL) CFRelease(executableURL);
    
    if (bundle->_isLoaded) {
        // Remove from the scheduled unload set if we are there.
        __CFSpinLock(&CFBundleGlobalDataLock);
        if (_bundlesToUnload) CFSetRemoveValue(_bundlesToUnload, bundle);
        __CFSpinUnlock(&CFBundleGlobalDataLock);
        return true;
    }

    // Unload bundles scheduled for unloading
    if (!_scheduledBundlesAreUnloading) _CFBundleUnloadScheduledBundles();
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
        case __CFBundleUnreadableBinary:
            result = _CFBundleCFMLoad(bundle, subError);
            break;
#elif defined(BINARY_SUPPORT_DLFCN)
        case __CFBundleUnreadableBinary:
            result = _CFBundleDlfcnLoadBundle(bundle, forceGlobal, subError);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (_useDlfcn) result = _CFBundleDlfcnLoadBundle(bundle, forceGlobal, subError); else
#endif /* BINARY_SUPPORT_DLFCN */
            result = _CFBundleDYLDLoadBundle(bundle, forceGlobal, subError);
            break;
        case __CFBundleDYLDFrameworkBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (_useDlfcn) result = _CFBundleDlfcnLoadFramework(bundle, subError); else
#endif /* BINARY_SUPPORT_DLFCN */
            result = _CFBundleDYLDLoadFramework(bundle, subError);
            break;
        case __CFBundleDYLDExecutableBinary:
            CFLog(__kCFLogBundle, CFSTR("Attempt to load executable of a type that cannot be dynamically loaded for %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotLoadableError);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLFCN)
        case __CFBundleUnknownBinary:
        case __CFBundleELFBinary:
            result = _CFBundleDlfcnLoadBundle(bundle, forceGlobal, subError);
            break;
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            result = _CFBundleDLLLoad(bundle, subError);
            break;
#endif /* BINARY_SUPPORT_DLL */
        case __CFBundleNoBinary:
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
            break;     
        default:
            CFLog(__kCFLogBundle, CFSTR("Cannot recognize type of executable for %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotLoadableError);
            break;
    }
    if (result && bundle->_plugInData._isPlugIn) _CFBundlePlugInLoaded(bundle);

    if (!result && error) *error = localError;
    return result;
}

Boolean CFBundleLoadExecutableAndReturnError(CFBundleRef bundle, CFErrorRef *error) {
    return _CFBundleLoadExecutableAndReturnError(bundle, false, error);
}

Boolean CFBundleLoadExecutable(CFBundleRef bundle) {
    return _CFBundleLoadExecutableAndReturnError(bundle, false, NULL);
}

Boolean CFBundlePreflightExecutable(CFBundleRef bundle, CFErrorRef *error) {
    Boolean result = false;
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

    if (!executableURL) bundle->_binaryType = __CFBundleNoBinary;
    // make sure we know whether bundle is already loaded or not
#if defined(BINARY_SUPPORT_DLFCN)
    if (!bundle->_isLoaded && _useDlfcn) _CFBundleDlfcnCheckLoaded(bundle);
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD)
    if (!bundle->_isLoaded) _CFBundleDYLDCheckLoaded(bundle);
    // We might need to figure out what it is
    if (bundle->_binaryType == __CFBundleUnknownBinary) {
        bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
        if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) bundle->_resourceData._executableLacksResourceFork = true;
#endif /* BINARY_SUPPORT_CFM */
    }
#endif /* BINARY_SUPPORT_DYLD */
    if (executableURL) CFRelease(executableURL);
    
    if (bundle->_isLoaded) return true;
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
        case __CFBundleUnreadableBinary:
            result = true;
            break;
#elif defined(BINARY_SUPPORT_DLFCN)
        case __CFBundleUnreadableBinary:
            result = _CFBundleDlfcnPreflight(bundle, subError);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
            result = true;
#if defined(BINARY_SUPPORT_DLFCN)
            result = _CFBundleDlfcnPreflight(bundle, subError);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
        case __CFBundleDYLDFrameworkBinary:
            result = true;
#if defined(BINARY_SUPPORT_DLFCN)
            result = _CFBundleDlfcnPreflight(bundle, subError);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
        case __CFBundleDYLDExecutableBinary:
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotLoadableError);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLFCN)
        case __CFBundleUnknownBinary:
        case __CFBundleELFBinary:
            result = _CFBundleDlfcnPreflight(bundle, subError);
            break;
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            result = true;
            break;
#endif /* BINARY_SUPPORT_DLL */
        case __CFBundleNoBinary:
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
            break;     
        default:
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotLoadableError);
            break;
    }
    if (!result && error) *error = localError;
    return result;
}

CFArrayRef CFBundleCopyExecutableArchitectures(CFBundleRef bundle) {
    CFArrayRef result = NULL;
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
    if (executableURL) {
        result = _CFBundleCopyArchitecturesForExecutable(executableURL);
        CFRelease(executableURL);
    }
    return result;
}

#if defined(BINARY_SUPPORT_DYLD)
static Boolean _CFBundleGetObjCImageInfo(CFBundleRef bundle, uint32_t *objcVersion, uint32_t *objcFlags) {
    Boolean retval = false;
    uint32_t localVersion = 0, localFlags = 0;
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
    if (executableURL) {
        retval = _CFBundleGetObjCImageInfoForExecutable(executableURL, &localVersion, &localFlags);
        CFRelease(executableURL);
    }
    if (objcVersion) *objcVersion = localVersion;
    if (objcFlags) *objcFlags = localFlags;
    return retval;
}
#endif /* defined(BINARY_SUPPORT_DYLD) */

void CFBundleUnloadExecutable(CFBundleRef bundle) {
    // First unload bundles scheduled for unloading (if that's not what we are already doing.)
    if (!_scheduledBundlesAreUnloading) _CFBundleUnloadScheduledBundles();
    
    if (!bundle->_isLoaded) return;
    
    // Remove from the scheduled unload set if we are there.
    if (!_scheduledBundlesAreUnloading) __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesToUnload) CFSetRemoveValue(_bundlesToUnload, bundle);
    if (!_scheduledBundlesAreUnloading) __CFSpinUnlock(&CFBundleGlobalDataLock);
    
    // Give the plugIn code a chance to realize this...
    _CFPlugInWillUnload(bundle);

    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
             _CFBundleCFMUnload(bundle);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) _CFBundleDlfcnUnload(bundle); else
#endif /* BINARY_SUPPORT_DLFCN */
            _CFBundleDYLDUnloadBundle(bundle);
            break;
        case __CFBundleDYLDFrameworkBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie && _CFExecutableLinkedOnOrAfter(CFSystemVersionLeopard)) _CFBundleDlfcnUnload(bundle);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            _CFBundleDLLUnload(bundle);
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) _CFBundleDlfcnUnload(bundle);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
    }
    if (!bundle->_isLoaded && bundle->_glueDict) {
        CFDictionaryApplyFunction(bundle->_glueDict, _CFBundleDeallocateGlue, (void *)CFGetAllocator(bundle));
        CFRelease(bundle->_glueDict);
        bundle->_glueDict = NULL;
    }
}

__private_extern__ void _CFBundleScheduleForUnloading(CFBundleRef bundle) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (!_bundlesToUnload) {
        // Create this from the default allocator
        CFSetCallBacks nonRetainingCallbacks = kCFTypeSetCallBacks;
        nonRetainingCallbacks.retain = NULL;
        nonRetainingCallbacks.release = NULL;
        _bundlesToUnload = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &nonRetainingCallbacks);
    }
    CFSetAddValue(_bundlesToUnload, bundle);
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ void _CFBundleUnscheduleForUnloading(CFBundleRef bundle) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesToUnload) {
        CFSetRemoveValue(_bundlesToUnload, bundle);
    }
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ void _CFBundleUnloadScheduledBundles(void) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesToUnload) {
        CFIndex c = CFSetGetCount(_bundlesToUnload);
        if (c > 0) {
            CFIndex i;
            CFBundleRef *unloadThese = (CFBundleRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(CFBundleRef) * c, 0);
            CFSetGetValues(_bundlesToUnload, (const void **)unloadThese);
            _scheduledBundlesAreUnloading = true;
            for (i = 0; i < c; i++) {
                // This will cause them to be removed from the set.  (Which is why we copied all the values out of the set up front.)
                CFBundleUnloadExecutable(unloadThese[i]);
            }
            _scheduledBundlesAreUnloading = false;
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, unloadThese);
        }
    }
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

void *CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef funcName) {
    void *tvp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            tvp = _CFBundleCFMGetSymbolByName(bundle, funcName, kTVectorCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) return _CFBundleDlfcnGetSymbolByName(bundle, funcName);
#endif /* BINARY_SUPPORT_DLFCN */
            return _CFBundleDYLDGetSymbolByName(bundle, funcName);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            tvp = _CFBundleDLLGetSymbolByName(bundle, funcName);
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) return _CFBundleDlfcnGetSymbolByName(bundle, funcName);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
    }
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM)
    if (tvp) {
        if (!bundle->_glueDict) bundle->_glueDict = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, NULL, NULL);
        void *fp = (void *)CFDictionaryGetValue(bundle->_glueDict, tvp);
        if (!fp) {
            fp = _CFBundleFunctionPointerForTVector(CFGetAllocator(bundle), tvp);
            CFDictionarySetValue(bundle->_glueDict, tvp, fp);
        }
        return fp;
    }
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM */
    return tvp;
}

void *_CFBundleGetCFMFunctionPointerForName(CFBundleRef bundle, CFStringRef funcName) {
    void *fp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
#if defined (BINARY_SUPPORT_CFM) || defined (BINARY_SUPPORT_DYLD) || defined (BINARY_SUPPORT_DLFCN)
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            return _CFBundleCFMGetSymbolByName(bundle, funcName, kTVectorCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) fp = _CFBundleDlfcnGetSymbolByNameWithSearch(bundle, funcName, true); else
#endif /* BINARY_SUPPORT_DLFCN */
            fp = _CFBundleDYLDGetSymbolByNameWithSearch(bundle, funcName, true);
            break;
#endif /* BINARY_SUPPORT_DYLD */
        default:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) fp = _CFBundleDlfcnGetSymbolByNameWithSearch(bundle, funcName, true);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
    }
#endif /* BINARY_SUPPORT_CFM || BINARY_SUPPORT_DYLD || BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM)
    if (fp) {
        if (!bundle->_glueDict) bundle->_glueDict = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, NULL, NULL);
        void *tvp = (void *)CFDictionaryGetValue(bundle->_glueDict, fp);
        if (!tvp) {
            tvp = _CFBundleTVectorForFunctionPointer(CFGetAllocator(bundle), fp);
            CFDictionarySetValue(bundle->_glueDict, fp, tvp);
        }
        return tvp;
    }
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM */
    return fp;
}

void CFBundleGetFunctionPointersForNames(CFBundleRef bundle, CFArrayRef functionNames, void *ftbl[]) {
    SInt32 i, c;

    if (!ftbl) return;

    c = CFArrayGetCount(functionNames);
    for (i = 0; i < c; i++) {
        ftbl[i] = CFBundleGetFunctionPointerForName(bundle, (CFStringRef)CFArrayGetValueAtIndex(functionNames, i));
    }
}

void _CFBundleGetCFMFunctionPointersForNames(CFBundleRef bundle, CFArrayRef functionNames, void *ftbl[]) {
    SInt32 i, c;

    if (!ftbl) return;

    c = CFArrayGetCount(functionNames);
    for (i = 0; i < c; i++) {
        ftbl[i] = _CFBundleGetCFMFunctionPointerForName(bundle, (CFStringRef)CFArrayGetValueAtIndex(functionNames, i));
    }
}

void *CFBundleGetDataPointerForName(CFBundleRef bundle, CFStringRef symbolName) {
    void *dp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            dp = _CFBundleCFMGetSymbolByName(bundle, symbolName, kDataCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) dp = _CFBundleDlfcnGetSymbolByName(bundle, symbolName); else
#endif /* BINARY_SUPPORT_DLFCN */
            dp = _CFBundleDYLDGetSymbolByName(bundle, symbolName);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            /* MF:!!! Handle this someday */
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
#if defined(BINARY_SUPPORT_DLFCN)
            if (bundle->_handleCookie) dp = _CFBundleDlfcnGetSymbolByName(bundle, symbolName);
#endif /* BINARY_SUPPORT_DLFCN */
            break;
    }
    return dp;
}

void CFBundleGetDataPointersForNames(CFBundleRef bundle, CFArrayRef symbolNames, void *stbl[]) {
    SInt32 i, c;

    if (!stbl) return;

    c = CFArrayGetCount(symbolNames);
    for (i = 0; i < c; i++) {
        stbl[i] = CFBundleGetDataPointerForName(bundle, (CFStringRef)CFArrayGetValueAtIndex(symbolNames, i));
    }
}

__private_extern__ _CFResourceData *__CFBundleGetResourceData(CFBundleRef bundle) {
    return &(bundle->_resourceData);
}

CFPlugInRef CFBundleGetPlugIn(CFBundleRef bundle) {
    if (bundle->_plugInData._isPlugIn) {
        return (CFPlugInRef)bundle;
    } else {
        return NULL;
    }
}

__private_extern__ _CFPlugInData *__CFBundleGetPlugInData(CFBundleRef bundle) {
    return &(bundle->_plugInData);
}

__private_extern__ Boolean _CFBundleCouldBeBundle(CFURLRef url) {
    Boolean result = false;
    Boolean exists;
    SInt32 mode;

    if (_CFGetFileProperties(kCFAllocatorSystemDefault, url, &exists, &mode, NULL, NULL, NULL, NULL) == 0) {
        result = (exists && ((mode & S_IFMT) == S_IFDIR) && ((mode & 0444) != 0));
    }
    return result;
}

#define LENGTH_OF(A) (sizeof(A) / sizeof(A[0]))

__private_extern__ CFURLRef _CFBundleCopyFrameworkURLForExecutablePath(CFAllocatorRef alloc, CFStringRef executablePath) {
    // MF:!!! Implement me.  We need to be able to find the bundle from the exe, dealing with old vs. new as well as the Executables dir business on Windows.
#if DEPLOYMENT_TARGET_WINDOWS
    UniChar executablesToFrameworksPathBuff[] = {'.', '.', '\\', 'F', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k', 's'};  // length 16
    UniChar executablesToPrivateFrameworksPathBuff[] = {'.', '.', '\\', 'P', 'r', 'i', 'v', 'a', 't', 'e', 'F', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k', 's'};  // length 23
    UniChar frameworksExtension[] = {'f', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k'};  // length 9
#endif

    UniChar pathBuff[CFMaxPathSize];
    UniChar nameBuff[CFMaxPathSize];
    CFIndex length, nameStart, nameLength, savedLength;
    CFMutableStringRef cheapStr = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, NULL, 0, 0, NULL);
    CFURLRef bundleURL = NULL;

    length = CFStringGetLength(executablePath);
    if (length > CFMaxPathSize) length = CFMaxPathSize;
    CFStringGetCharacters(executablePath, CFRangeMake(0, length), pathBuff);

    // Save the name in nameBuff
    length = _CFLengthAfterDeletingPathExtension(pathBuff, length);
    nameStart = _CFStartOfLastPathComponent(pathBuff, length);
    nameLength = length - nameStart;
    memmove(nameBuff, &(pathBuff[nameStart]), nameLength * sizeof(UniChar));

    // Strip the name from pathBuff
    length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
    savedLength = length;

#if DEPLOYMENT_TARGET_WINDOWS
    // * (Windows-only) First check the "Executables" directory parallel to the "Frameworks" directory case.
    if (_CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, executablesToFrameworksPathBuff, 16) && _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, nameBuff, nameLength) && _CFAppendPathExtension(pathBuff, &length, CFMaxPathSize, frameworksExtension, 9)) {
        CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
        bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
        if (!_CFBundleCouldBeBundle(bundleURL)) {
            CFRelease(bundleURL);
            bundleURL = NULL;
        }
    }
    // * (Windows-only) Next check the "Executables" directory parallel to the "PrivateFrameworks" directory case.
    if (bundleURL == NULL) {
        length = savedLength;
        if (_CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, executablesToPrivateFrameworksPathBuff, 23) && _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, nameBuff, nameLength) && _CFAppendPathExtension(pathBuff, &length, CFMaxPathSize, frameworksExtension, 9)) {
            CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
            bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
            if (!_CFBundleCouldBeBundle(bundleURL)) {
                CFRelease(bundleURL);
                bundleURL = NULL;
            }
        }
    }
#endif

    // * Finally check the executable inside the framework case.
    if (!bundleURL) {
        // MF:!!! This should ensure the framework name is the same as the library name!
        CFIndex curStart;
        
        length = savedLength;
        // To catch all the cases, we just peel off level looking for one ending in .framework or one called "Supporting Files".

        while (length > 0) {
            curStart = _CFStartOfLastPathComponent(pathBuff, length);
            if (curStart >= length) {
                break;
            }
            CFStringSetExternalCharactersNoCopy(cheapStr, &(pathBuff[curStart]), length - curStart, CFMaxPathSize - curStart);
            if (CFEqual(cheapStr, _CFBundleSupportFilesDirectoryName1) || CFEqual(cheapStr, _CFBundleSupportFilesDirectoryName2)) {
                length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
                CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
                bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
                if (!_CFBundleCouldBeBundle(bundleURL)) {
                    CFRelease(bundleURL);
                    bundleURL = NULL;
                }
                break;
            } else if (CFStringHasSuffix(cheapStr, CFSTR(".framework"))) {
                CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
                bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
                if (!_CFBundleCouldBeBundle(bundleURL)) {
                    CFRelease(bundleURL);
                    bundleURL = NULL;
                }
                break;
            }
            length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
        }
    }

    CFStringSetExternalCharactersNoCopy(cheapStr, NULL, 0, 0);
    CFRelease(cheapStr);

    return bundleURL;
}

static void _CFBundleEnsureBundleExistsForImagePath(CFStringRef imagePath) {
    // This finds the bundle for the given path.
    // If an image path corresponds to a bundle, we see if there is already a bundle instance.  If there is and it is NOT in the _dynamicBundles array, it is added to the staticBundles.  Do not add the main bundle to the list here.
    CFBundleRef bundle;
    CFURLRef curURL = _CFBundleCopyFrameworkURLForExecutablePath(kCFAllocatorSystemDefault, imagePath);
    Boolean doFinalProcessing = false;

    if (curURL) {
        bundle = _CFBundleFindByURL(curURL, true);
        if (!bundle) {
            bundle = _CFBundleCreate(kCFAllocatorSystemDefault, curURL, true, false);
            doFinalProcessing = true;
        }
        if (bundle && !bundle->_isLoaded) {
            // make sure that these bundles listed as loaded, and mark them frameworks (we probably can't see anything else here, and we cannot unload them)
#if defined(BINARY_SUPPORT_DLFCN)
            if (!bundle->_isLoaded && _useDlfcn) _CFBundleDlfcnCheckLoaded(bundle);
#endif /* BINARY_SUPPORT_DLFCN */
#if defined(BINARY_SUPPORT_DYLD)
            if (bundle->_binaryType == __CFBundleUnknownBinary) bundle->_binaryType = __CFBundleDYLDFrameworkBinary;
            if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) bundle->_resourceData._executableLacksResourceFork = true;
            if (!bundle->_isLoaded) _CFBundleDYLDCheckLoaded(bundle);
#endif /* BINARY_SUPPORT_DYLD */
#if LOG_BUNDLE_LOAD
            if (!bundle->_isLoaded) printf("ensure bundle %p set loaded fallback, handle %p image %p conn %p\n", bundle, bundle->_handleCookie, bundle->_imageCookie, bundle->_connectionCookie);
#endif /* LOG_BUNDLE_LOAD */
            bundle->_isLoaded = true;
        }
        // Perform delayed final processing steps.
        // This must be done after _isLoaded has been set.
        if (bundle && doFinalProcessing) {
            _CFBundleCheckWorkarounds(bundle);
            if (_CFBundleNeedsInitPlugIn(bundle)) {
                __CFSpinUnlock(&CFBundleGlobalDataLock);
                _CFBundleInitPlugIn(bundle);
                __CFSpinLock(&CFBundleGlobalDataLock);
            }
        }
        CFRelease(curURL);
    }
}

static void _CFBundleEnsureBundlesExistForImagePaths(CFArrayRef imagePaths) {
    // This finds the bundles for the given paths.
    // If an image path corresponds to a bundle, we see if there is already a bundle instance.  If there is and it is NOT in the _dynamicBundles array, it is added to the staticBundles.  Do not add the main bundle to the list here (even if it appears in imagePaths).
    CFIndex i, imagePathCount = CFArrayGetCount(imagePaths);

    for (i = 0; i < imagePathCount; i++) {
        _CFBundleEnsureBundleExistsForImagePath((CFStringRef)CFArrayGetValueAtIndex(imagePaths, i));
    }
}

static void _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(CFStringRef hint) {
    CFArrayRef imagePaths = NULL;
    // Tickle the main bundle into existence
    (void)_CFBundleGetMainBundleAlreadyLocked();
#if defined(BINARY_SUPPORT_DYLD)
    imagePaths = _CFBundleDYLDCopyLoadedImagePathsForHint(hint);
#endif /* BINARY_SUPPORT_DYLD */
    if (imagePaths) {
        _CFBundleEnsureBundlesExistForImagePaths(imagePaths);
        CFRelease(imagePaths);
    }
}

static void _CFBundleEnsureAllBundlesUpToDateAlreadyLocked(void) {
    // This method returns all the statically linked bundles.  This includes the main bundle as well as any frameworks that the process was linked against at launch time.  It does not include frameworks or opther bundles that were loaded dynamically.
    CFArrayRef imagePaths = NULL;

    // Tickle the main bundle into existence
    (void)_CFBundleGetMainBundleAlreadyLocked();

#if defined(BINARY_SUPPORT_DLL)
// Dont know how to find static bundles for DLLs
#endif /* BINARY_SUPPORT_DLL */

#if defined(BINARY_SUPPORT_CFM)
// CFM bundles are supplied to us by CFM, so we do not need to figure them out ourselves
#endif /* BINARY_SUPPORT_CFM */

#if defined(BINARY_SUPPORT_DYLD)
    imagePaths = _CFBundleDYLDCopyLoadedImagePathsIfChanged();
#endif /* BINARY_SUPPORT_DYLD */
    if (imagePaths) {
        _CFBundleEnsureBundlesExistForImagePaths(imagePaths);
        CFRelease(imagePaths);
    }
}

CFArrayRef CFBundleGetAllBundles(void) {
    // To answer this properly, we have to have created the static bundles!
    __CFSpinLock(&CFBundleGlobalDataLock);
    _CFBundleEnsureAllBundlesUpToDateAlreadyLocked();
    __CFSpinUnlock(&CFBundleGlobalDataLock);
    return _allBundles;
}

uint8_t _CFBundleLayoutVersion(CFBundleRef bundle) {return bundle->_version;}

CF_EXPORT CFURLRef _CFBundleCopyInfoPlistURL(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFURLRef url = (CFURLRef)CFDictionaryGetValue(infoDict, _kCFBundleInfoPlistURLKey);
    if (!url) url = (CFURLRef)CFDictionaryGetValue(infoDict, _kCFBundleRawInfoPlistURLKey);
    return (url ? (CFURLRef)CFRetain(url) : NULL);
}

CF_EXPORT CFURLRef _CFBundleCopyPrivateFrameworksURL(CFBundleRef bundle) {return CFBundleCopyPrivateFrameworksURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopyPrivateFrameworksURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase0, bundle->_url);
    }
    return result;
}

CF_EXPORT CFURLRef _CFBundleCopySharedFrameworksURL(CFBundleRef bundle) {return CFBundleCopySharedFrameworksURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopySharedFrameworksURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase0, bundle->_url);
    }
    return result;
}

CF_EXPORT CFURLRef _CFBundleCopySharedSupportURL(CFBundleRef bundle) {return CFBundleCopySharedSupportURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopySharedSupportURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase0, bundle->_url);
    }
    return result;
}

__private_extern__ CFURLRef _CFBundleCopyBuiltInPlugInsURL(CFBundleRef bundle) {return CFBundleCopyBuiltInPlugInsURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopyBuiltInPlugInsURL(CFBundleRef bundle) {
    CFURLRef result = NULL, alternateResult = NULL;

    CFAllocatorRef alloc = CFGetAllocator(bundle);
    if (1 == bundle->_version) {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase0, bundle->_url);
    }
    if (!result || !_urlExists(alloc, result)) {
        if (1 == bundle->_version) {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase1, bundle->_url);
        } else if (2 == bundle->_version) {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase2, bundle->_url);
        } else {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase0, bundle->_url);
        }
        if (alternateResult && _urlExists(alloc, alternateResult)) {
            if (result) CFRelease(result);
            result = alternateResult;
        } else {
            if (alternateResult) CFRelease(alternateResult);
        }
    }
    return result;
}



#if defined(BINARY_SUPPORT_DYLD)

static const void *__CFBundleDYLDFindImage(char *buff) {
    const void *header = NULL;
    uint32_t i, numImages = _dyld_image_count(), numMatches = 0;
    const char *curName, *p, *q;

    for (i = 0; !header && i < numImages; i++) {
        curName = _dyld_get_image_name(i);
        if (curName && 0 == strncmp(curName, buff, CFMaxPathSize)) {
            header = _dyld_get_image_header(i);
            numMatches = 1;
        }
    }
    if (!header) {
        for (i = 0; i < numImages; i++) {
            curName = _dyld_get_image_name(i);
            if (curName) {
                for (p = buff, q = curName; *p && *q && (q - curName < CFMaxPathSize); p++, q++) {
                    if (*p != *q && (q - curName > 11) && 0 == strncmp(q - 11, ".framework/Versions/", 20) && *(q + 9) && '/' == *(q + 10)) q += 11;
                    else if (*p != *q && (q - curName > 12) && 0 == strncmp(q - 12, ".framework/Versions/", 20) && *(q + 8) && '/' == *(q + 9)) q += 10;
                    if (*p != *q) break;
                }
                if (*p == *q) {
                    header = _dyld_get_image_header(i);
                    numMatches++;
                }
            }
        }
    }
    return (numMatches == 1) ? header : NULL;
}

__private_extern__ Boolean _CFBundleDYLDCheckLoaded(CFBundleRef bundle) {
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];

        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            const void *header = __CFBundleDYLDFindImage(buff);
            if (header) {
                if (bundle->_binaryType == __CFBundleUnknownBinary) bundle->_binaryType = __CFBundleDYLDFrameworkBinary;
                if (!bundle->_imageCookie) {
                    bundle->_imageCookie = header;
#if LOG_BUNDLE_LOAD
                    printf("dyld check load bundle %p, find %s getting image %p\n", bundle, buff, bundle->_imageCookie);
#endif /* LOG_BUNDLE_LOAD */
                }
                bundle->_isLoaded = true;
            } else {
#if LOG_BUNDLE_LOAD
                printf("dyld check load bundle %p, find %s no image\n", bundle, buff);
#endif /* LOG_BUNDLE_LOAD */
            }
        }
        if (executableURL) CFRelease(executableURL);
    }
    return bundle->_isLoaded;
}

__private_extern__ Boolean _CFBundleDYLDLoadBundle(CFBundleRef bundle, Boolean forceGlobal, CFErrorRef *error) {
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    NSLinkEditErrors c = NSLinkEditUndefinedError;
    int errorNumber = 0;
    const char *fileName = NULL;
    const char *errorString = NULL;

    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];

        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            NSObjectFileImage image;
            NSObjectFileImageReturnCode retCode = NSCreateObjectFileImageFromFile(buff, &image);
#if LOG_BUNDLE_LOAD
            printf("dyld load bundle %p, create image of %s returns image %p retcode %d\n", bundle, buff, image, retCode);
#endif /* LOG_BUNDLE_LOAD */
            if (retCode == NSObjectFileImageSuccess) {
                uint32_t options = forceGlobal ? NSLINKMODULE_OPTION_RETURN_ON_ERROR : (NSLINKMODULE_OPTION_BINDNOW | NSLINKMODULE_OPTION_PRIVATE | NSLINKMODULE_OPTION_RETURN_ON_ERROR);
                NSModule module = NSLinkModule(image, buff, options);
#if LOG_BUNDLE_LOAD
                printf("dyld load bundle %p, link module of %s options 0x%x returns module %p image %p\n", bundle, buff, options, module, image);
#endif /* LOG_BUNDLE_LOAD */
                if (module) {
                    bundle->_imageCookie = image;
                    bundle->_moduleCookie = module;
                    bundle->_isLoaded = true;
                } else {
                    NSLinkEditError(&c, &errorNumber, &fileName, &errorString);
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s:  error code %d, error number %d (%s)"), fileName, c, errorNumber, errorString);
                    if (error) {
#if defined(BINARY_SUPPORT_DLFCN)
                        _CFBundleDlfcnPreflight(bundle, subError);
#endif /* BINARY_SUPPORT_DLFCN */
                        if (!localError) {
                            CFStringRef debugString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("error code %d, error number %d (%s)"), c, errorNumber, errorString);
                            localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError, debugString);
                            CFRelease(debugString);
                        }
                    }
                    (void)NSDestroyObjectFileImage(image);
                }
            } else {
                CFLog(__kCFLogBundle, CFSTR("dyld returns %d when trying to load %@"), retCode, executableURL);
                if (error) {
                    if (retCode == NSObjectFileImageArch) {
                        localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableArchitectureMismatchError);
                    } else if (retCode == NSObjectFileImageInappropriateFile) {
                        Boolean hasRuntimeMismatch = false;
                        uint32_t mainFlags = 0, bundleFlags = 0;
                        if (_CFBundleGrokObjCImageInfoFromMainExecutable(NULL, &mainFlags) && (mainFlags & 0x2) != 0) {
                            if (_CFBundleGetObjCImageInfo(bundle, NULL, &bundleFlags) && (bundleFlags & 0x2) == 0) hasRuntimeMismatch = true;
                        }
                        if (hasRuntimeMismatch) {
                            localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableRuntimeMismatchError);
                        } else {
                            localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotLoadableError);
                        }
                    } else {
#if defined(BINARY_SUPPORT_DLFCN)
                        _CFBundleDlfcnPreflight(bundle, subError);
#endif /* BINARY_SUPPORT_DLFCN */
                        if (!localError) {
                            CFStringRef debugString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("dyld returns %d"), retCode);
                            localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError, debugString);
                            CFRelease(debugString);
                        }
                    }
                }
            }
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!bundle->_isLoaded && error) *error = localError;
    return bundle->_isLoaded;
}

__private_extern__ Boolean _CFBundleDYLDLoadFramework(CFBundleRef bundle, CFErrorRef *error) {
    // !!! Framework loading should be better.  Can't unload frameworks.
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    NSLinkEditErrors c = NSLinkEditUndefinedError;
    int errorNumber = 0;
    const char *fileName = NULL;
    const char *errorString = NULL;

    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];

        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            void *image = (void *)NSAddImage(buff, NSADDIMAGE_OPTION_RETURN_ON_ERROR);
#if LOG_BUNDLE_LOAD
            printf("dyld load framework %p, add image of %s returns image %p\n", bundle, buff, image);
#endif /* LOG_BUNDLE_LOAD */
            if (image) {
                bundle->_imageCookie = image;
                bundle->_isLoaded = true;
            } else {
                NSLinkEditError(&c, &errorNumber, &fileName, &errorString);
                CFLog(__kCFLogBundle, CFSTR("Error loading %s:  error code %d, error number %d (%s)"), fileName, c, errorNumber, errorString);
                if (error) {
#if defined(BINARY_SUPPORT_DLFCN)
                    _CFBundleDlfcnPreflight(bundle, subError);
#endif /* BINARY_SUPPORT_DLFCN */
                    if (!localError) {
                        CFStringRef debugString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("error code %d, error number %d (%s)"), c, errorNumber, errorString);
                        localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError, debugString);
                        CFRelease(debugString);
                    }
                }
            }
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!bundle->_isLoaded && error) *error = localError;
    return bundle->_isLoaded;
}

__private_extern__ void _CFBundleDYLDUnloadBundle(CFBundleRef bundle) {
    if (bundle->_isLoaded) {
#if LOG_BUNDLE_LOAD
        printf("dyld unload bundle %p, handle %p module %p image %p\n", bundle, bundle->_handleCookie, bundle->_moduleCookie, bundle->_imageCookie);
#endif /* LOG_BUNDLE_LOAD */
        if (bundle->_moduleCookie && !NSUnLinkModule((NSModule)(bundle->_moduleCookie), NSUNLINKMODULE_OPTION_NONE)) {
            CFLog(__kCFLogBundle, CFSTR("Internal error unloading bundle %@"), bundle);
        } else {
            if (bundle->_moduleCookie && bundle->_imageCookie) (void)NSDestroyObjectFileImage((NSObjectFileImage)(bundle->_imageCookie));
            bundle->_connectionCookie = bundle->_handleCookie = NULL;
            bundle->_imageCookie = bundle->_moduleCookie = NULL;
            bundle->_isLoaded = false;
        }
    }
}

__private_extern__ void *_CFBundleDYLDGetSymbolByName(CFBundleRef bundle, CFStringRef symbolName) {return _CFBundleDYLDGetSymbolByNameWithSearch(bundle, symbolName, false);}

static void *_CFBundleDYLDGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch) {
    void *result = NULL;
    char buff[1026];
    NSSymbol symbol = NULL;
    
    buff[0] = '_';
    if (CFStringGetCString(symbolName, &(buff[1]), 1024, kCFStringEncodingUTF8)) {
        if (bundle->_moduleCookie) {
            symbol = NSLookupSymbolInModule((NSModule)(bundle->_moduleCookie), buff);
        } else if (bundle->_imageCookie) {
            symbol = NSLookupSymbolInImage((const struct mach_header*)bundle->_imageCookie, buff, NSLOOKUPSYMBOLINIMAGE_OPTION_BIND|NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR);
        } 
        if (!symbol && !bundle->_moduleCookie && (!bundle->_imageCookie || globalSearch)) {
            char hintBuff[1026];
            CFStringRef executableName = _CFBundleCopyExecutableName(kCFAllocatorSystemDefault, bundle, NULL, NULL);
            hintBuff[0] = '\0';
            if (executableName) {
                if (!CFStringGetCString(executableName, hintBuff, 1024, kCFStringEncodingUTF8)) hintBuff[0] = '\0';
                CFRelease(executableName);
            }
	    // Nowdays, NSIsSymbolNameDefinedWithHint() and NSLookupAndBindSymbolWithHint()
	    // are identical, except the first just returns a bool, so checking with the
	    // Is function first just causes a redundant lookup.
	    // This returns NULL on failure.
            symbol = NSLookupAndBindSymbolWithHint(buff, hintBuff);
        }
        if (symbol) result = NSAddressOfSymbol(symbol);
#if defined(DEBUG)
        if (!result) CFLog(__kCFLogBundle, CFSTR("dyld cannot find symbol %s in %@"), buff, bundle);
#endif /* DEBUG */
#if LOG_BUNDLE_LOAD
        printf("bundle %p handle %p module %p image %p dyld returns symbol %p for %s\n", bundle, bundle->_handleCookie, bundle->_moduleCookie, bundle->_imageCookie, result, buff + 1);
#endif /* LOG_BUNDLE_LOAD */
    }
    return result;
}

static CFStringRef _CFBundleDYLDCopyLoadedImagePathForPointer(void *p) {
    CFStringRef result = NULL;
    if (!result) {
        uint32_t i, j, n = _dyld_image_count();
        Boolean foundit = false;
        const char *name;
#if __LP64__
#define MACH_HEADER_TYPE struct mach_header_64
#define MACH_SEGMENT_CMD_TYPE struct segment_command_64
#define MACH_SEGMENT_FLAVOR LC_SEGMENT_64
#else
#define MACH_HEADER_TYPE struct mach_header
#define MACH_SEGMENT_CMD_TYPE struct segment_command
#define MACH_SEGMENT_FLAVOR LC_SEGMENT
#endif
        for (i = 0; !foundit && i < n; i++) {
            const MACH_HEADER_TYPE *mh = (const MACH_HEADER_TYPE *)_dyld_get_image_header(i);
            uintptr_t addr = (uintptr_t)p - _dyld_get_image_vmaddr_slide(i);
            if (mh) {
                struct load_command *lc = (struct load_command *)((char *)mh + sizeof(MACH_HEADER_TYPE));
                for (j = 0; !foundit && j < mh->ncmds; j++, lc = (struct load_command *)((char *)lc + lc->cmdsize)) {
                    if (MACH_SEGMENT_FLAVOR == lc->cmd && ((MACH_SEGMENT_CMD_TYPE *)lc)->vmaddr <= addr && addr < ((MACH_SEGMENT_CMD_TYPE *)lc)->vmaddr + ((MACH_SEGMENT_CMD_TYPE *)lc)->vmsize) {
                        foundit = true;
                        name = _dyld_get_image_name(i);
                        if (name) result = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, name);
                    }
                }
            }
        }
#undef MACH_HEADER_TYPE
#undef MACH_SEGMENT_CMD_TYPE
#undef MACH_SEGMENT_FLAVOR
    }
#if LOG_BUNDLE_LOAD
    printf("dyld image path for pointer %p is %p\n", p, result);
#endif /* LOG_BUNDLE_LOAD */
    return result;
}

__private_extern__ CFArrayRef _CFBundleDYLDCopyLoadedImagePathsForHint(CFStringRef hint) {
    uint32_t i, numImages = _dyld_image_count();
    CFMutableArrayRef result = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFRange range = CFRangeMake(0, CFStringGetLength(hint));
    const char *processPath = _CFProcessPath();
    
    for (i = 0; i < numImages; i++) {
        const char *curName = _dyld_get_image_name(i), *lastComponent = NULL;
        if (curName && (!processPath || 0 != strcmp(curName, processPath))) lastComponent = strrchr(curName, '/');
        if (lastComponent) {
            CFStringRef str = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, lastComponent + 1);
            if (str) {
                if (CFStringFindWithOptions(hint, str, range, kCFCompareAnchored|kCFCompareBackwards|kCFCompareCaseInsensitive, NULL)) {
                    CFStringRef curStr = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, curName);
                    if (curStr) {
                        CFArrayAppendValue(result, curStr);
                        CFRelease(curStr);
                    }
                }
                CFRelease(str);
            }
        }
    }
    return result;
}

static char *_cleanedPathForPath(const char *curName) {
    char *thePath = strdup(curName);
    if (thePath) {
        // We are going to process the buffer replacing all "/./" and "//" with "/"
        CFIndex srcIndex = 0, dstIndex = 0;
        CFIndex len = strlen(thePath);
        for (srcIndex=0; srcIndex<len; srcIndex++) {
            thePath[dstIndex] = thePath[srcIndex];
            dstIndex++;
            while (srcIndex < len-1 && thePath[srcIndex] == '/' && (thePath[srcIndex+1] == '/' || (thePath[srcIndex+1] == '.' && srcIndex < len-2 && thePath[srcIndex+2] == '/'))) srcIndex += (thePath[srcIndex+1] == '/' ? 1 : 2);
        }
        thePath[dstIndex] = 0;
    }
    return thePath;
}

__private_extern__ CFArrayRef _CFBundleDYLDCopyLoadedImagePathsIfChanged(void) {
    // This returns an array of the paths of all the dyld images in the process.  These paths may not be absolute, they may point at things that are not bundles, they may be staticly linked bundles or dynamically loaded bundles, they may be NULL.
    static uint32_t _cachedDYLDImageCount = -1;

    uint32_t i, numImages = _dyld_image_count();
    CFMutableArrayRef result = NULL;

    if (numImages != _cachedDYLDImageCount) {
        const char *curName;
        char *cleanedCurName = NULL;
        CFStringRef curStr;
        const char *processPath = _CFProcessPath();

        result = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);

        for (i = 0; i < numImages; i++) {
            curName = _dyld_get_image_name(i);
            if (curName && i == 0) cleanedCurName = _cleanedPathForPath(curName);
            if (curName && (!processPath || 0 != strcmp(curName, processPath)) && (!processPath || !cleanedCurName || 0 != strcmp(cleanedCurName, processPath))) {
                curStr = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, curName);
                if (curStr) {
                    CFArrayAppendValue(result, curStr);
                    CFRelease(curStr);
                }
            }
            if (cleanedCurName) {
                free(cleanedCurName);
                cleanedCurName = NULL;
            }
        }
        _cachedDYLDImageCount = numImages;
    }
    return result;
}

#endif /* BINARY_SUPPORT_DYLD */

#if defined(BINARY_SUPPORT_DLFCN)

__private_extern__ Boolean _CFBundleDlfcnCheckLoaded(CFBundleRef bundle) {
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];

        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            int mode = RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD | CF_RTLD_FIRST;
            void *handle = dlopen(buff, mode);
            if (handle) {
                if (!bundle->_handleCookie) {
                    bundle->_handleCookie = handle;
#if LOG_BUNDLE_LOAD
                    printf("dlfcn check load bundle %p, dlopen of %s mode 0x%x getting handle %p\n", bundle, buff, mode, bundle->_handleCookie);
#endif /* LOG_BUNDLE_LOAD */
                }
                bundle->_isLoaded = true;
            } else {
#if LOG_BUNDLE_LOAD
                printf("dlfcn check load bundle %p, dlopen of %s mode 0x%x no handle\n", bundle, buff, mode);
#endif /* LOG_BUNDLE_LOAD */
            }
        }
        if (executableURL) CFRelease(executableURL);
    }
    return bundle->_isLoaded;
}

static SInt32 _CFBundleCurrentArchitecture(void) {
    SInt32 arch = 0;
#if defined(__ppc__) || defined(__powerpc__)
    arch = kCFBundleExecutableArchitecturePPC;
#elif defined(__ppc64__)
    arch = kCFBundleExecutableArchitecturePPC64;
#elif defined(__i386__)
    arch = kCFBundleExecutableArchitectureI386;
#elif defined(__x86_64__)
    arch = kCFBundleExecutableArchitectureX86_64;
#elif defined(BINARY_SUPPORT_DYLD)
    const NXArchInfo *archInfo = NXGetLocalArchInfo();
    if (archInfo) arch = archInfo->cputype;
#endif
    return arch;
}

extern Boolean _CFBundleDlfcnPreflight(CFBundleRef bundle, CFErrorRef *error) {
    Boolean retval = true;
    CFErrorRef localError = NULL;
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];
        
        retval = false;
        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
#if DEPLOYMENT_TARGET_MACOSX && (MAC_OS_X_VERSION_10_5 <= MAC_OS_X_VERSION_MAX_ALLOWED)
            retval = dlopen_preflight(buff);
#else
			retval = false;
#endif /* DEPLOYMENT_TARGET_MACOSX && (MAC_OS_X_VERSION_10_5 <= MAC_OS_X_VERSION_MAX_ALLOWED) */
            if (!retval && error) {
                CFArrayRef archs = CFBundleCopyExecutableArchitectures(bundle);
                CFStringRef debugString = NULL;
                const char *errorString = dlerror();
                if (errorString && strlen(errorString) > 0) debugString = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, errorString);
                if (archs) {
                    Boolean hasSuitableArch = false, hasRuntimeMismatch = false;
                    CFIndex i, count = CFArrayGetCount(archs);
                    SInt32 arch, curArch = _CFBundleCurrentArchitecture();
                    for (i = 0; !hasSuitableArch && i < count; i++) {
                        if (CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(archs, i), kCFNumberSInt32Type, (void *)&arch) && arch == curArch) hasSuitableArch = true;
                    }
#if defined(BINARY_SUPPORT_DYLD)
                    if (hasSuitableArch) {
                        uint32_t mainFlags = 0, bundleFlags = 0;
                        if (_CFBundleGrokObjCImageInfoFromMainExecutable(NULL, &mainFlags) && (mainFlags & 0x2) != 0) {
                            if (_CFBundleGetObjCImageInfo(bundle, NULL, &bundleFlags) && (bundleFlags & 0x2) == 0) hasRuntimeMismatch = true;
                        }
                    }
#endif /* BINARY_SUPPORT_DYLD */
                    if (hasRuntimeMismatch) {
                        localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableRuntimeMismatchError, debugString);
                    } else if (!hasSuitableArch) {
                        localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableArchitectureMismatchError, debugString);
                    } else {
                        localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLoadError, debugString);
                    }
                    CFRelease(archs);
                } else {
                    localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLoadError, debugString);
                }
                if (debugString) CFRelease(debugString);
            }
        } else {
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!retval && error) *error = localError;
    return retval;
}

__private_extern__ Boolean _CFBundleDlfcnLoadBundle(CFBundleRef bundle, Boolean forceGlobal, CFErrorRef *error) {
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];
        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            int mode = forceGlobal ? (RTLD_LAZY | RTLD_GLOBAL | CF_RTLD_FIRST) : (RTLD_NOW | RTLD_LOCAL | CF_RTLD_FIRST);
            bundle->_handleCookie = dlopen(buff, mode);
#if LOG_BUNDLE_LOAD
            printf("dlfcn load bundle %p, dlopen of %s mode 0x%x returns handle %p\n", bundle, buff, mode, bundle->_handleCookie);
#endif /* LOG_BUNDLE_LOAD */
            if (bundle->_handleCookie) {
                bundle->_isLoaded = true;
            } else {
                CFStringRef debugString = NULL;
                const char *errorString = dlerror();
                if (errorString) {
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s:  %s"), buff, errorString);
                    debugString = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, errorString);
                } else {
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s"), buff);
                }
                if (error && _CFBundleDlfcnPreflight(bundle, subError)) localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError, debugString);
                if (debugString) CFRelease(debugString);
            }
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!bundle->_isLoaded && error) *error = localError;
    return bundle->_isLoaded;
}

__private_extern__ Boolean _CFBundleDlfcnLoadFramework(CFBundleRef bundle, CFErrorRef *error) {
    CFErrorRef localError = NULL, *subError = (error ? &localError : NULL);
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        char buff[CFMaxPathSize];
        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            int mode = RTLD_LAZY | RTLD_GLOBAL | CF_RTLD_FIRST;
            bundle->_handleCookie = dlopen(buff, mode);
#if LOG_BUNDLE_LOAD
            printf("dlfcn load framework %p, dlopen of %s mode 0x%x returns handle %p\n", bundle, buff, mode, bundle->_handleCookie);
#endif /* LOG_BUNDLE_LOAD */
            if (bundle->_handleCookie) {
                bundle->_isLoaded = true;
            } else {
                CFStringRef debugString = NULL;
                const char *errorString = dlerror();
                if (errorString) {
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s:  %s"), buff, errorString);
                    debugString = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, errorString);
                } else {
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s"), buff);
                }
                if (error && _CFBundleDlfcnPreflight(bundle, subError)) localError = _CFBundleCreateErrorDebug(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError, debugString);
                if (debugString) CFRelease(debugString);
            }
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!bundle->_isLoaded && error) *error = localError;
    return bundle->_isLoaded;
}

__private_extern__ void _CFBundleDlfcnUnload(CFBundleRef bundle) {
    if (bundle->_isLoaded) {
#if LOG_BUNDLE_LOAD
        printf("dlfcn unload bundle %p, handle %p module %p image %p\n", bundle, bundle->_handleCookie, bundle->_moduleCookie, bundle->_imageCookie);
#endif /* LOG_BUNDLE_LOAD */
        if (0 != dlclose(bundle->_handleCookie)) {
            CFLog(__kCFLogBundle, CFSTR("Internal error unloading bundle %@"), bundle);
        } else {
            bundle->_connectionCookie = bundle->_handleCookie = NULL;
            bundle->_imageCookie = bundle->_moduleCookie = NULL;
            bundle->_isLoaded = false;
        }
    }
}

__private_extern__ void *_CFBundleDlfcnGetSymbolByName(CFBundleRef bundle, CFStringRef symbolName) {return _CFBundleDlfcnGetSymbolByNameWithSearch(bundle, symbolName, false);}

static void *_CFBundleDlfcnGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch) {
    void *result = NULL;
    char buff[1026];
    
    if (CFStringGetCString(symbolName, buff, 1024, kCFStringEncodingUTF8)) {
        result = dlsym(bundle->_handleCookie, buff);
        if (!result && globalSearch) result = dlsym(RTLD_DEFAULT, buff);
#if defined(DEBUG)
        if (!result) CFLog(__kCFLogBundle, CFSTR("dlsym cannot find symbol %s in %@"), buff, bundle);
#endif /* DEBUG */
#if LOG_BUNDLE_LOAD
        printf("bundle %p handle %p module %p image %p dlsym returns symbol %p for %s\n", bundle, bundle->_handleCookie, bundle->_moduleCookie, bundle->_imageCookie, result, buff);
#endif /* LOG_BUNDLE_LOAD */
    }
    return result;
}

static CFStringRef _CFBundleDlfcnCopyLoadedImagePathForPointer(void *p) {
    CFStringRef result = NULL;
    Dl_info info;
    if (0 != dladdr(p, &info) && info.dli_fname) result = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, info.dli_fname);
#if LOG_BUNDLE_LOAD
    printf("dlfcn image path for pointer %p is %p\n", p, result);
#endif /* LOG_BUNDLE_LOAD */
    return result;
}

#endif /* BINARY_SUPPORT_DLFCN */


#if defined(BINARY_SUPPORT_DLL)

__private_extern__ Boolean _CFBundleDLLLoad(CFBundleRef bundle, CFErrorRef *error) {
    CFErrorRef localError = NULL;
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
        TCHAR buff[CFMaxPathSize];

        if (executableURL && CFURLGetFileSystemRepresentation(executableURL, true, (uint8_t *)buff, CFMaxPathSize)) {
            bundle->_hModule = LoadLibrary(buff);
            if (bundle->_hModule) {
                bundle->_isLoaded = true;
            } else {
                if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableLinkError);
            }
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
            if (error) localError = _CFBundleCreateError(CFGetAllocator(bundle), bundle, CFBundleExecutableNotFoundError);
        }
        if (executableURL) CFRelease(executableURL);
    }
    if (!bundle->_isLoaded && error) *error = localError;
    return bundle->_isLoaded;
}

__private_extern__ void _CFBundleDLLUnload(CFBundleRef bundle) {
    if (bundle->_isLoaded) {
       FreeLibrary(bundle->_hModule);
       bundle->_hModule = NULL;
       bundle->_isLoaded = false;
    }
}

__private_extern__ void *_CFBundleDLLGetSymbolByName(CFBundleRef bundle, CFStringRef symbolName) {
    void *result = NULL;
    char buff[1024];
    if (CFStringGetCString(symbolName, buff, 1024, kCFStringEncodingWindowsLatin1)) result = GetProcAddress(bundle->_hModule, buff);
    return result;
}

#endif /* BINARY_SUPPORT_DLL */

/* Workarounds to be applied in the presence of certain bundles can go here. This is called on every bundle creation.
*/
extern void _CFStringSetCompatibility(CFOptionFlags);

static void _CFBundleCheckWorkarounds(CFBundleRef bundle) {
}

