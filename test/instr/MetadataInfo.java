/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.io.*;
import java.util.*;

import com.facebook.proguard.annotations.DoNotStrip;

@DoNotStrip
public class MetadataInfo {
    @DoNotStrip private int name;
    @DoNotStrip private int offset;
    @DoNotStrip private int numBlocks;
    @DoNotStrip private int numVectors;
    @DoNotStrip private HashMap<Integer, Integer> block2BitMap = new HashMap<>();
    @DoNotStrip private HashMap<Integer, Integer> bit2BlockMap = new HashMap<>();
    @DoNotStrip private HashMap<Integer, String> block2SourceBlockMap = new HashMap<>();

    public MetadataInfo () {
        name = 0;
        offset = 0;
        numBlocks = 0;
        numVectors = 0;
    }

    @DoNotStrip
    public MetadataInfo (int index, int off, int blocks, int vectors, String bit2BlockString, String bit2SourceBlock) {
        name = index;
        offset = off;
        numBlocks = blocks;
        numVectors = vectors;

        if (numVectors != 0) {
            String[] bit2BlockList = bit2BlockString.split(";");
            String[] bit2SourceList = bit2SourceBlock.split(";");

            int idx = 0;
            for (String blockString : bit2BlockList ) {
                int blockNum = Integer.parseInt(blockString);
                bit2BlockMap.put(idx, blockNum);
                block2BitMap.put(blockNum, idx);
                idx += 1;
            }

            idx = 0;
            for (String sourceString : bit2SourceList ) {
                block2SourceBlockMap.put(bit2BlockMap.get(idx), sourceString);
                idx += 1;
            }
        }
    }

    @DoNotStrip
    public int getName () {
        return name;
    }

    @DoNotStrip
    public int getOffset () {
        return offset;
    }

    @DoNotStrip
    public int getNumBlocks () {
        return numBlocks;
    }

    @DoNotStrip
    public int getNumVectors () {
        return name;
    }

    @DoNotStrip
    public int getBlockBit (int block) {
        return block2BitMap.get(block);
    }

    @DoNotStrip
    public int[] getBlockFromVectorIndices (ArrayList<Integer> al) {
        int[] blockList = new int[al.size()];
        int idx = 0;
        for(int index : al) {
            blockList[idx] = bit2BlockMap.get(idx);
            idx += 1;
        }

        return blockList;
    }

}
