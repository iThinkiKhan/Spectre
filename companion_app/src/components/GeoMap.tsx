import React, {useEffect, useMemo, useState} from 'react';
import {
  Image,
  NativeModules,
  Pressable,
  StyleSheet,
  Text,
  View,
  type LayoutChangeEvent,
} from 'react-native';

import type {
  LocalizationEstimate,
  LocalizationObservation,
} from '../services/localization/LocalizationService';
import type {ActiveLocationFix} from '../state/SpectreContext';
import {theme} from '../theme/theme';

type Props = {
  estimate: LocalizationEstimate | null;
  observations: LocalizationObservation[];
  phoneLocation: ActiveLocationFix | null;
};

const TILE_SIZE = 256;

type SpectreMapTileModule = {
  getTile(zoom: number, x: number, y: number): Promise<string>;
};

const mapTiles = NativeModules.SpectreMapTiles as SpectreMapTileModule;

function clampLat(lat: number) {
  return Math.max(-85.05112878, Math.min(85.05112878, lat));
}

function worldPoint(lat: number, lon: number, zoom: number) {
  const scale = TILE_SIZE * 2 ** zoom;
  const safeLat = clampLat(lat);
  const sin = Math.sin((safeLat * Math.PI) / 180);
  return {
    x: ((lon + 180) / 360) * scale,
    y:
      (0.5 - Math.log((1 + sin) / (1 - sin)) / (4 * Math.PI)) *
      scale,
  };
}

export function GeoMap({estimate, observations, phoneLocation}: Props) {
  const [zoom, setZoom] = useState(16);
  const [size, setSize] = useState({width: 0, height: 0});
  const [tileUris, setTileUris] = useState<Record<string, string>>({});
  const center = estimate ?? phoneLocation ?? observations[0] ?? null;

  const map = useMemo(() => {
    if (!center || size.width <= 0 || size.height <= 0) return null;
    const centerWorld = worldPoint(center.lat, center.lon, zoom);
    const centerTileX = Math.floor(centerWorld.x / TILE_SIZE);
    const centerTileY = Math.floor(centerWorld.y / TILE_SIZE);
    const tileCount = 2 ** zoom;
    const xRadius = Math.ceil(size.width / TILE_SIZE / 2) + 1;
    const yRadius = Math.ceil(size.height / TILE_SIZE / 2) + 1;
    const tiles: Array<{key: string; x: number; y: number; left: number; top: number}> = [];
    for (let dy = -yRadius; dy <= yRadius; dy += 1) {
      for (let dx = -xRadius; dx <= xRadius; dx += 1) {
        const rawX = centerTileX + dx;
        const y = centerTileY + dy;
        if (y < 0 || y >= tileCount) continue;
        const x = ((rawX % tileCount) + tileCount) % tileCount;
        tiles.push({
          key: `${zoom}-${x}-${y}-${rawX}`,
          x,
          y,
          left: rawX * TILE_SIZE - centerWorld.x + size.width / 2,
          top: y * TILE_SIZE - centerWorld.y + size.height / 2,
        });
      }
    }
    const position = (lat: number, lon: number) => {
      const point = worldPoint(lat, lon, zoom);
      return {
        left: point.x - centerWorld.x + size.width / 2,
        top: point.y - centerWorld.y + size.height / 2,
      };
    };
    return {tiles, position};
  }, [center, size, zoom]);

  useEffect(() => {
    let active = true;
    const missingTiles = (map?.tiles ?? []).filter(tile => !tileUris[tile.key]);
    void Promise.all(
      missingTiles.map(async tile => {
        try {
          return {key: tile.key, uri: await mapTiles.getTile(zoom, tile.x, tile.y)};
        } catch {
          // The observation overlay remains usable when no network is present.
          return null;
        }
      }),
    ).then(loaded => {
      if (!active) return;
      setTileUris(previous => {
        const next = {...previous};
        loaded.forEach(tile => {
          if (tile) next[tile.key] = tile.uri;
        });
        return next;
      });
    });
    return () => {
      active = false;
    };
    // tileUris is intentionally sampled when the visible tile set changes;
    // including it would restart every outstanding download after each result.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [map?.tiles, zoom]);

  const onLayout = (event: LayoutChangeEvent) => {
    const {width, height} = event.nativeEvent.layout;
    setSize({width, height});
  };
  const metersPerPixel = center
    ? (156543.03392 * Math.cos((center.lat * Math.PI) / 180)) / 2 ** zoom
    : 1;
  const radiusPx = estimate
    ? Math.max(18, Math.min(180, estimate.uncertaintyM / metersPerPixel))
    : 0;

  return (
    <View style={styles.frame} onLayout={onLayout}>
      {!center && (
        <View style={styles.empty}>
          <Text style={styles.emptyTitle}>No mapped observations yet</Text>
          <Text style={styles.emptyText}>
            Start a mission and offload Spectre after it has collected GPS-tagged radio observations.
          </Text>
        </View>
      )}
      {map?.tiles.map(tile => {
        const uri = tileUris[tile.key];
        return uri ? (
          <Image
            key={tile.key}
            source={{uri}}
            style={[styles.tile, {left: tile.left, top: tile.top}]}
          />
        ) : null;
      })}

      {map && estimate && (
        <View
          pointerEvents="none"
          style={[
            styles.uncertainty,
            {
              width: radiusPx * 2,
              height: radiusPx * 2,
              borderRadius: radiusPx,
              left: map.position(estimate.lat, estimate.lon).left - radiusPx,
              top: map.position(estimate.lat, estimate.lon).top - radiusPx,
            },
          ]}
        />
      )}
      {map && observations.slice(0, 64).map(observation => {
        const point = map.position(observation.lat, observation.lon);
        const strength = Math.max(5, Math.min(13, 13 - (-35 - observation.rssi) / 8));
        return (
          <View
              key={`${observation.sensorId}-${observation.eventId}-${observation.timestamp}`}
            pointerEvents="none"
            style={[
              styles.observation,
              {
                width: strength,
                height: strength,
                borderRadius: strength / 2,
                left: point.left - strength / 2,
                top: point.top - strength / 2,
              },
            ]}
          />
        );
      })}
      {map && estimate && (() => {
        const point = map.position(estimate.lat, estimate.lon);
        return <View pointerEvents="none" style={[styles.estimate, {left: point.left - 8, top: point.top - 8}]} />;
      })()}
      {map && phoneLocation && (() => {
        const point = map.position(phoneLocation.lat, phoneLocation.lon);
        return <View pointerEvents="none" style={[styles.phone, {left: point.left - 6, top: point.top - 6}]} />;
      })()}

      {!!center && (
        <>
          <View style={styles.zoomControls}>
            <Pressable style={styles.zoomButton} onPress={() => setZoom(value => Math.min(19, value + 1))}>
              <Text style={styles.zoomText}>+</Text>
            </Pressable>
            <Pressable style={styles.zoomButton} onPress={() => setZoom(value => Math.max(12, value - 1))}>
              <Text style={styles.zoomText}>−</Text>
            </Pressable>
          </View>
          <Text style={styles.attribution}>© OpenStreetMap contributors</Text>
        </>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  frame: {
    height: 390,
    overflow: 'hidden',
    borderRadius: theme.radius.lg,
    borderColor: theme.colors.panelEdgeBright,
    borderWidth: 1,
    backgroundColor: '#111713',
  },
  tile: {position: 'absolute', width: TILE_SIZE, height: TILE_SIZE},
  empty: {flex: 1, alignItems: 'center', justifyContent: 'center', padding: 28, gap: 8},
  emptyTitle: {color: theme.colors.textStrong, fontSize: 18, fontWeight: '700'},
  emptyText: {color: theme.colors.textSoft, textAlign: 'center', lineHeight: 20},
  observation: {position: 'absolute', backgroundColor: theme.colors.cyan, borderColor: '#003b40', borderWidth: 1},
  estimate: {position: 'absolute', width: 16, height: 16, borderRadius: 8, backgroundColor: theme.colors.amber, borderColor: '#000', borderWidth: 3},
  phone: {position: 'absolute', width: 12, height: 12, borderRadius: 6, backgroundColor: theme.colors.lime, borderColor: '#003820', borderWidth: 2},
  uncertainty: {position: 'absolute', backgroundColor: 'rgba(252,231,0,0.16)', borderColor: theme.colors.amber, borderWidth: 2},
  zoomControls: {position: 'absolute', right: 10, top: 10, gap: 6},
  zoomButton: {width: 38, height: 38, borderRadius: 10, backgroundColor: 'rgba(0,0,0,0.82)', alignItems: 'center', justifyContent: 'center', borderColor: '#666', borderWidth: 1},
  zoomText: {color: '#fff', fontSize: 23, lineHeight: 26, fontWeight: '700'},
  attribution: {position: 'absolute', right: 6, bottom: 5, color: '#111', backgroundColor: 'rgba(255,255,255,0.78)', paddingHorizontal: 4, fontSize: 9},
});
