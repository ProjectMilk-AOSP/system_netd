/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Netd"

#include "PhysicalNetwork.h"

#include "RouteController.h"
#include "SockDiag.h"

#include "log/log.h"

namespace android::net {

namespace {

[[nodiscard]] int addToDefault(unsigned netId, const std::string& interface, Permission permission,
                               PhysicalNetwork::Delegate* delegate) {
    if (int ret = RouteController::addInterfaceToDefaultNetwork(interface.c_str(), permission)) {
        ALOGE("failed to add interface %s to default netId %u", interface.c_str(), netId);
        return ret;
    }
    if (int ret = delegate->addFallthrough(interface, permission)) {
        return ret;
    }
    return 0;
}

[[nodiscard]] int removeFromDefault(unsigned netId, const std::string& interface,
                                    Permission permission, PhysicalNetwork::Delegate* delegate) {
    if (int ret = RouteController::removeInterfaceFromDefaultNetwork(interface.c_str(),
                                                                     permission)) {
        ALOGE("failed to remove interface %s from default netId %u", interface.c_str(), netId);
        return ret;
    }
    if (int ret = delegate->removeFallthrough(interface, permission)) {
        return ret;
    }
    return 0;
}

}  // namespace

PhysicalNetwork::Delegate::~Delegate() {}

PhysicalNetwork::PhysicalNetwork(unsigned netId, PhysicalNetwork::Delegate* delegate, bool local)
    : Network(netId),
      mDelegate(delegate),
      mPermission(PERMISSION_NONE),
      mIsDefault(false),
      mIsLocalNetwork(local) {}

PhysicalNetwork::~PhysicalNetwork() {}

Permission PhysicalNetwork::getPermission() const {
    return mPermission;
}

int PhysicalNetwork::destroySocketsLackingPermission(Permission permission) {
    if (permission == PERMISSION_NONE) return 0;

    SockDiag sd;
    if (!sd.open()) {
       ALOGE("Error closing sockets for netId %d permission change", mNetId);
       return -EBADFD;
    }
    if (int ret = sd.destroySocketsLackingPermission(mNetId, permission,
                                                     true /* excludeLoopback */)) {
        ALOGE("Failed to close sockets changing netId %d to permission %d: %s",
              mNetId, permission, strerror(-ret));
        return ret;
    }
    return 0;
}

void PhysicalNetwork::invalidateRouteCache(const std::string& interface) {
    // This method invalidates all socket destination cache entries in the kernel by creating and
    // removing a low-priority route.
    // This number is an arbitrary number that need to be higher than any other route created either
    // by netd or by an IPv6 RouterAdvertisement.
    int priority = 100000;

    for (const auto& dst : { "0.0.0.0/0", "::/0" }) {
        // If any of these operations fail, there's no point in logging because RouteController will
        // have already logged a message. There's also no point returning an error since there's
        // nothing we can do.
        (void)RouteController::addRoute(interface.c_str(), dst, "throw", RouteController::INTERFACE,
                                        0 /* mtu */, priority);
        (void)RouteController::removeRoute(interface.c_str(), dst, "throw",
                                           RouteController::INTERFACE, priority);
    }
}

int PhysicalNetwork::setPermission(Permission permission) {
    if (permission == mPermission) {
        return 0;
    }
    if (mInterfaces.empty()) {
        mPermission = permission;
        return 0;
    }

    destroySocketsLackingPermission(permission);
    for (const std::string& interface : mInterfaces) {
        if (int ret = RouteController::modifyPhysicalNetworkPermission(
                    mNetId, interface.c_str(), mPermission, permission, mIsLocalNetwork)) {
            ALOGE("failed to change permission on interface %s of netId %u from %x to %x",
                  interface.c_str(), mNetId, mPermission, permission);
            return ret;
        }
        invalidateRouteCache(interface);
    }
    if (mIsDefault) {
        for (const std::string& interface : mInterfaces) {
            if (int ret = addToDefault(mNetId, interface, permission, mDelegate)) {
                return ret;
            }
            if (int ret = removeFromDefault(mNetId, interface, mPermission, mDelegate)) {
                return ret;
            }
        }
    }
    // Destroy sockets again in case any were opened after we called destroySocketsLackingPermission
    // above and before we changed the permissions. These sockets won't be able to send any RST
    // packets because they are now no longer routed, but at least the apps will get errors.
    destroySocketsLackingPermission(permission);
    mPermission = permission;
    return 0;
}

int PhysicalNetwork::addAsDefault() {
    if (mIsDefault) {
        return 0;
    }
    for (const std::string& interface : mInterfaces) {
        if (int ret = addToDefault(mNetId, interface, mPermission, mDelegate)) {
            return ret;
        }
    }
    mIsDefault = true;
    return 0;
}

int PhysicalNetwork::removeAsDefault() {
    if (!mIsDefault) {
        return 0;
    }
    for (const std::string& interface : mInterfaces) {
        if (int ret = removeFromDefault(mNetId, interface, mPermission, mDelegate)) {
            return ret;
        }
    }
    mIsDefault = false;
    return 0;
}

int PhysicalNetwork::addUsers(const UidRanges& uidRanges, int32_t subPriority) {
    if (!isValidSubPriority(subPriority) || !canAddUidRanges(uidRanges)) {
        return -EINVAL;
    }

    for (const std::string& interface : mInterfaces) {
        int ret = RouteController::addUsersToPhysicalNetwork(
                mNetId, interface.c_str(), {{subPriority, uidRanges}}, mIsLocalNetwork);
        if (ret) {
            ALOGE("failed to add users on interface %s of netId %u", interface.c_str(), mNetId);
            return ret;
        }
    }
    addToUidRangeMap(uidRanges, subPriority);
    return 0;
}

int PhysicalNetwork::removeUsers(const UidRanges& uidRanges, int32_t subPriority) {
    if (!isValidSubPriority(subPriority)) return -EINVAL;

    for (const std::string& interface : mInterfaces) {
        int ret = RouteController::removeUsersFromPhysicalNetwork(
                mNetId, interface.c_str(), {{subPriority, uidRanges}}, mIsLocalNetwork);
        if (ret) {
            ALOGE("failed to remove users on interface %s of netId %u", interface.c_str(), mNetId);
            return ret;
        }
    }
    removeFromUidRangeMap(uidRanges, subPriority);
    return 0;
}

int PhysicalNetwork::addInterface(const std::string& interface) {
    if (hasInterface(interface)) {
        return 0;
    }
    if (int ret = RouteController::addInterfaceToPhysicalNetwork(
                mNetId, interface.c_str(), mPermission, mUidRangeMap, mIsLocalNetwork)) {
        ALOGE("failed to add interface %s to netId %u", interface.c_str(), mNetId);
        return ret;
    }
    if (mIsDefault) {
        if (int ret = addToDefault(mNetId, interface, mPermission, mDelegate)) {
            return ret;
        }
    }
    mInterfaces.insert(interface);
    return 0;
}

int PhysicalNetwork::removeInterface(const std::string& interface) {
    if (!hasInterface(interface)) {
        return 0;
    }
    if (mIsDefault) {
        if (int ret = removeFromDefault(mNetId, interface, mPermission, mDelegate)) {
            return ret;
        }
    }
    // This step will flush the interface index from the cache in RouteController so it must be
    // done last as further requests to the RouteController regarding this interface will fail
    // to find the interface index in the cache in cases where the interface is already gone
    // (e.g. bt-pan).
    if (int ret = RouteController::removeInterfaceFromPhysicalNetwork(
                mNetId, interface.c_str(), mPermission, mUidRangeMap, mIsLocalNetwork)) {
        ALOGE("failed to remove interface %s from netId %u", interface.c_str(), mNetId);
        return ret;
    }
    mInterfaces.erase(interface);
    return 0;
}

bool PhysicalNetwork::isValidSubPriority(int32_t priority) {
    // SUB_PRIORITY_NO_DEFAULT is a special value, see UidRanges.h.
    return (priority >= UidRanges::SUB_PRIORITY_HIGHEST &&
            priority <= UidRanges::SUB_PRIORITY_LOWEST) ||
           priority == UidRanges::SUB_PRIORITY_NO_DEFAULT;
}

}  // namespace android::net
