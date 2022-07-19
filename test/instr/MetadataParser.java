/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.io.InputStream;
import java.util.Scanner;
import java.util.HashMap;
import java.util.ArrayList;

import com.facebook.proguard.annotations.DoNotStrip;

import com.facebook.redextest.MetadataInfo;

@DoNotStrip
public class MetadataParser{
    @DoNotStrip private static InputStream metadataFile;
    @DoNotStrip private static InputStream sourceBlockFile;
    @DoNotStrip private static HashMap<String, Integer> indexMap = new HashMap<>();
    @DoNotStrip private static HashMap<Integer, MetadataInfo> metadataMap = new HashMap<>();

    // Populate index HashMap that provides every Method with a number (index)
    @DoNotStrip
    public static void populateIndexMap () {
        Scanner myReader = new Scanner(sourceBlockFile);
        myReader.nextLine();
        myReader.nextLine();
        myReader.nextLine();

        while (myReader.hasNextLine()) {
            String data = myReader.nextLine();
            String[] lst = data.split(",");
            String temp = lst[1];
            temp = temp.split(";.")[1];
            String funcName = temp.split(":")[0];
            int index = Integer.parseInt(lst[0]);
            indexMap.put(funcName, index);
        }
        myReader.close();
    }

    // Populate metadata HashMap by reading redex-instrument-medata.csv and base the parsing
    // on whether it is Basic Block Tracing or Basic Block Hit Count
    @DoNotStrip
    public static void populateMetadataMap (boolean isTracing) {
        Scanner myReader = new Scanner(metadataFile);
        myReader.nextLine();
        myReader.nextLine();
        myReader.nextLine();

        while (myReader.hasNextLine()) {
            String data = myReader.nextLine();
            String[] lst = data.split(",");
            int offset = Integer.parseInt(lst[0]);
            int index = Integer.parseInt(lst[1]);
            int blocks = Integer.parseInt(lst[3]);
            int vectors = Integer.parseInt(lst[4]);
            String bitVector = "";
            String srcVector = "";
            if (lst.length > 5) {
                bitVector = lst[5];
            }
            if (isTracing) {
                if (lst.length > 7) {
                    srcVector = lst[7];
                }

                MetadataInfo mI = new MetadataInfo(index, offset, blocks, vectors, bitVector, srcVector);
                metadataMap.put(index, mI);
            }
            else {
                int hitOffset = Integer.parseInt(lst[6]);
                int hitBlocks = Integer.parseInt(lst[7]);
                String hitVector = "";
                if (lst.length > 8) {
                    hitVector = lst[8];
                }
                if (lst.length > 10) {
                    srcVector = lst[10];
                }
                if (hitBlocks == 0) {
                    hitOffset = -1;
                }

                MetadataInfo mI = new MetadataInfo(index, offset, blocks, vectors, bitVector, hitOffset, hitBlocks, hitVector, srcVector);
                metadataMap.put(index, mI);
            }
        }
        myReader.close();
    }

    @DoNotStrip
    public static int getOffset (String funcName) {
        int output = -1;
        if (indexMap.containsKey(funcName)) {
            int index = indexMap.get(funcName);

            if (metadataMap.containsKey(index)) {
                MetadataInfo mi = metadataMap.get(index);

                output = mi.getOffset();
            }
        }
        return output;
    }

    @DoNotStrip
    public static int getHitOffset (String funcName) {
        int output = -1;
        if (indexMap.containsKey(funcName)) {
            int index = indexMap.get(funcName);

            if (metadataMap.containsKey(index)) {
                MetadataInfo mi = metadataMap.get(index);

                output = mi.getHitOffset();
            }
        }
        return output;
    }

    @DoNotStrip
    public static int checkBlockHit (String funcName, short[] stats, int block) {
        int completed = -1;
        int blockIndex = -1;
        int offset = -1;

        if (indexMap.containsKey(funcName)) {
            int index = indexMap.get(funcName);

            if (metadataMap.containsKey(index)) {
                MetadataInfo mi = metadataMap.get(index);

                blockIndex = mi.getBlockBit(block);
                offset = mi.getOffset();
            }
        }


        if (blockIndex == -1 || offset == -1) {
            return completed;
        }

        offset += 2 + (blockIndex / 16);
        int bit = blockIndex % 16;
        int bitvector = stats[offset];
        completed = (bitvector >> bit) & 1;

        return completed;
    }

    @DoNotStrip
    public static int checkBlockNumHits (String funcName, short[] stats, int[] hits, int block) {
        int completed = -1;
        int blockIndex = -1;
        int blockHitIndex = -1;
        int offset = -1;
        int hitOffset = -1;

        if (indexMap.containsKey(funcName)) {
            int index = indexMap.get(funcName);

            if (metadataMap.containsKey(index)) {
                MetadataInfo mi = metadataMap.get(index);

                blockIndex = mi.getBlockBit(block);
                blockHitIndex = mi.getBlockHitIndex(block);
                offset = mi.getOffset();
                hitOffset = mi.getHitOffset();
            }
        }


        if (blockIndex == -1 || offset == -1) {
            return completed;
        }

        if (blockHitIndex == -1) {
            int methodCount = stats[offset];
            offset += 2 + (blockIndex / 16);
            int bit = blockIndex % 16;
            int bitvector = stats[offset];
            completed = ((bitvector >> bit) & 1) * methodCount;
        }
        else {
            hitOffset += blockHitIndex;
            completed = hits[hitOffset];
        }


        return completed;
    }



    @DoNotStrip
    public static void startUp (InputStream  iMetadata, InputStream iSourceBlock, boolean isTracing) {
        sourceBlockFile = iSourceBlock;
        metadataFile = iMetadata;

        populateIndexMap();
        populateMetadataMap(isTracing);
    }
}
