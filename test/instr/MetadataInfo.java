/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.HashMap;
import java.util.ArrayList;

import com.facebook.proguard.annotations.DoNotStrip;

@DoNotStrip
public class MetadataInfo {
    @DoNotStrip private int name;
    @DoNotStrip private int offset;
    @DoNotStrip private int hitOffset;
    @DoNotStrip private int numBlocks;
    @DoNotStrip private int numVectors;
    @DoNotStrip private int numHitBlocks;
    @DoNotStrip private HashMap<Integer, Integer> block2HitMap = new HashMap<>();
    @DoNotStrip private HashMap<Integer, Integer> block2BitMap = new HashMap<>();
    @DoNotStrip private HashMap<Integer, Integer> bit2BlockMap = new HashMap<>();
    @DoNotStrip private HashMap<Integer, String> block2SourceBlockMap = new HashMap<>();

    public MetadataInfo () {
        name = 0;
        offset = -1;
        hitOffset = -1;
        numBlocks = 0;
        numVectors = 0;
        numHitBlocks = 0;
    }

    // Constructor for Basic Block Tracing
    @DoNotStrip
    public MetadataInfo (int index, int off, int blocks, int vectors, String bit2BlockString, String bit2SourceBlock) {
        name = index;
        offset = off;
        hitOffset = -1;
        numBlocks = blocks;
        numVectors = vectors;
        numHitBlocks = 0;

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

    // Constructor for Basic Block Hit Count
    @DoNotStrip
    public MetadataInfo (int index, int off, int blocks, int vectors, String bit2BlockString, int hitOff, int hitBlocks, String hit2BlockString, String bit2SourceBlock) {
        name = index;
        offset = off;
        hitOffset = hitOff;
        numBlocks = blocks;
        numVectors = vectors;
        numHitBlocks = hitBlocks;

        if (numVectors != 0) {
            String[] bit2BlockList = bit2BlockString.split(";");
            String[] bit2SourceList = bit2SourceBlock.split(";");
            String[] hit2BlockList = hit2BlockString.split(";");

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

            if (numHitBlocks != 0) {
                idx = 0;
                for (String blockString : hit2BlockList ) {
                    int blockNum = Integer.parseInt(blockString);
                    block2HitMap.put(blockNum, idx);
                    idx += 1;
                }
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
    public int getHitOffset () {
        return hitOffset;
    }

    @DoNotStrip
    public int getNumBlocks () {
        return numBlocks;
    }

    @DoNotStrip
    public int getNumVectors () {
        return numVectors;
    }

    @DoNotStrip
    public int getBlockBit (int block) {
        return block2BitMap.get(block);
    }

    @DoNotStrip
    public int getBlockHitIndex (int block) {
        Integer hitIndex = block2HitMap.get(block);
        if (hitIndex == null) {
            return -1;
        }
        return hitIndex;
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
