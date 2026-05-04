import {PermissionsAndroid, Platform} from 'react-native';

export type AndroidBlePermissionState = {
  checked: boolean;
  allGranted: boolean;
  missing: string[];
  statuses: Record<string, boolean>;
};

function requiredPermissions() {
  if (Platform.OS !== 'android') {
    return [];
  }

  if (Platform.Version >= 31) {
    return [
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_ADVERTISE,
      PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
    ];
  }

  return [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];
}

export async function readAndroidBlePermissions(): Promise<AndroidBlePermissionState> {
  const permissions = requiredPermissions();
  const statuses = Object.fromEntries(
    await Promise.all(
      permissions.map(async permission => [
        permission,
        await PermissionsAndroid.check(permission),
      ]),
    ),
  );

  const missing = permissions.filter(permission => !statuses[permission]);

  return {
    checked: true,
    allGranted: missing.length === 0,
    missing,
    statuses,
  };
}

export async function requestAndroidBlePermissions(): Promise<AndroidBlePermissionState> {
  const permissions = requiredPermissions();

  if (!permissions.length) {
    return {
      checked: true,
      allGranted: true,
      missing: [],
      statuses: {},
    };
  }

  const result = await PermissionsAndroid.requestMultiple(permissions);
  const statuses = Object.fromEntries(
    permissions.map(permission => [
      permission,
      result[permission] === PermissionsAndroid.RESULTS.GRANTED,
    ]),
  );
  const missing = permissions.filter(permission => !statuses[permission]);

  return {
    checked: true,
    allGranted: missing.length === 0,
    missing,
    statuses,
  };
}

export function friendlyPermissionName(permission: string) {
  switch (permission) {
    case PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN:
      return 'Bluetooth scan';
    case PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT:
      return 'Bluetooth connect';
    case PermissionsAndroid.PERMISSIONS.BLUETOOTH_ADVERTISE:
      return 'Bluetooth advertise';
    case PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION:
      return 'Precise location';
    default:
      return permission.split('.').pop() || permission;
  }
}
