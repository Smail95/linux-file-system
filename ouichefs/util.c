#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>

#include "util.h"
#include "ouichefs.h"
#include "bitmap.h"

/*
 * Check if there is any shared blocks before making any modifications.
 * If so, allocate a new block, copy data into it, decrease link count 
 * of the shared block. 
 * Called in:
 * - file.c: ouichefs_write_begin();
 * 
 * On success, 0 is returned.
 */
int ouichefs_cow(struct file *file, uint32_t vb_index, uint32_t nr_towrite)
{
	struct super_block *sb = file->f_inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(file->f_inode);
	struct ouichefs_file_index_block *index;
	struct ouichefs_block_info *binfo;
	struct buffer_head *bh_index = NULL, *bh_info = NULL;
	struct buffer_head *bh_old = NULL, *bh_new = NULL;
	uint32_t binfo_nr, binfo_shift, bno;
	int ret = 0;
	pr_info("-> ouichefs_cow\n");
	
	bh_index = sb_bread(file->f_inode->i_sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;
	
	/* Check link count of every block to write, copy it if nlink > 1 */
	while(nr_towrite--){
		bno = index->blocks[vb_index];
		binfo_nr = OUICHEFS_BINFO_INDEX(sbi, bno) / OUICHEFS_BINFO_PER_BLOCK + \
				OUICHEFS_FIRST_BINFO_STORE(sbi);
		binfo_shift = OUICHEFS_BINFO_INDEX(sbi, bno) % OUICHEFS_BINFO_PER_BLOCK;
		/* read block_info of bno */
		bh_info = sb_bread(sb, binfo_nr);
		if(!bh_info){
			ret = -EIO;
			goto BRELSE_INDEX;
		}
		binfo = (struct ouichefs_block_info *)bh_info->b_data;
		binfo += binfo_shift;	
				
		if(binfo->b_nlink == 1){
			goto  NEXT;
		}
		/* alloc new block for vb_index */
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto BRELSE_INFO;
		}
		ouichefs_link_block(sb, bno);
		
		/* read source block */
		bh_old = sb_bread(sb, index->blocks[vb_index]);
		if(!bh_old){
			ret = -EIO;
			put_block(sbi, bno);
			goto BRELSE_INFO;
		}
		/* copy data to new allocated block */
		bh_new = sb_bread(sb, bno);
		if(!bh_new){
			ret = -EIO;
			goto BRELSE_OLD;
		}
		memcpy(bh_new->b_data, bh_old->b_data, OUICHEFS_BLOCK_SIZE);
		pr_info("-- old_block[%u] -> new_block[%u]\n", index->blocks[vb_index], bno);
		
		/* decrement link count & ref new block */
		ouichefs_unlink_block(sb, index->blocks[vb_index]);
		index->blocks[vb_index] = bno;
		
		/* sync & brelse */
		mark_buffer_dirty(bh_info);
		mark_buffer_dirty(bh_index);
		mark_buffer_dirty(bh_new);

		brelse(bh_old);
		brelse(bh_new);	
NEXT:		brelse(bh_info);
		vb_index++;
	}
	goto BRELSE_INDEX;
	
BRELSE_OLD:
	brelse(bh_old);	
BRELSE_INFO:
	brelse(bh_info);
BRELSE_INDEX:
	brelse(bh_index);
	return ret;
}
/*
 * Increase the link count of the block.
 * Called in: 
 * - file.c: ouichefs_file_get_block();
 * - util.c: deduplicate_file();
 * 
 * On success, 0 is returned.
 */
int ouichefs_link_block(struct super_block *sb, uint32_t bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_block_info *binfo;
	struct buffer_head *bh = NULL; 
	uint32_t binfo_nr = OUICHEFS_BINFO_INDEX(sbi, bno) / OUICHEFS_BINFO_PER_BLOCK + \
				OUICHEFS_FIRST_BINFO_STORE(sbi);
	uint32_t binfo_shift = OUICHEFS_BINFO_INDEX(sbi, bno) % OUICHEFS_BINFO_PER_BLOCK;
	pr_info("-> ouichefs_link_block\n");
	
	/* Read binfo from disk */
	bh = sb_bread(sb, binfo_nr);
	if (!bh) {
		return -EIO;
	}
	binfo = (struct ouichefs_block_info *)bh->b_data;
	binfo += binfo_shift;
	
	/* Increment link count of the block and memory usage if deduplication occurs */
	if(binfo->b_nlink++ > 0)
		sbi->nr_free_blocks--;
	pr_info("-- block_count[%u -> %u] (binfo: %u, shift: %u)\n", bno, binfo->b_nlink, binfo_nr, binfo_shift);
	
	mark_buffer_dirty(bh);
	brelse(bh);
	
	return 0;
}
/*
 * Decrease link count of the block. if it reaches zero, put the block.
 * Called in:
 * - inode.c: ouichefs_unlink();
 * - util.c : deduplicate_file();
 * 
 * On success, the number of link count is returned. 
 */
int ouichefs_unlink_block(struct super_block *sb, uint32_t bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_block_info *binfo;
	struct buffer_head *bh = NULL; 
	uint32_t binfo_block = OUICHEFS_BINFO_INDEX(sbi, bno) / OUICHEFS_BINFO_PER_BLOCK + \
				OUICHEFS_FIRST_BINFO_STORE(sbi);
	uint32_t binfo_shift = OUICHEFS_BINFO_INDEX(sbi, bno) % OUICHEFS_BINFO_PER_BLOCK;
	uint32_t nlink;
	pr_info("-> ouichefs_unlink_block\n");
	
	/* Read block info from disk */
	bh = sb_bread(sb, binfo_block);
	if (!bh) {
		return -EIO;
	}
	binfo = (struct ouichefs_block_info *)bh->b_data;
	binfo += binfo_shift;
	
	/* Decrement b_nlink of the block if not null */
	if((nlink = binfo->b_nlink))
		nlink = --binfo->b_nlink;
	/* Put block if link count is 0, or ++ number of free block */
	if(!nlink)
		put_block(sbi, bno);
	else
		sbi->nr_free_blocks++;
	pr_info("-- block_count[%u -> %u]\n", bno, nlink);
	
	mark_buffer_dirty(bh);
	brelse(bh);
	return nlink;
}

/* 
 * Iterate over the blocks that make up the inode 'dup' and try to find
 * identical block content in the inode 'src'. If so, unlink the duplicated
 * blocks, reference the origin ones and increase link count of those blocks.
 * Called in:
 * - util.c: ouichefs_deduplicate();
 * 
 * On success, the number of blocks deduplicated is returned.
 */
int deduplicate_file(struct inode *src, struct inode *dup)
{
	struct ouichefs_file_index_block *src_iblock, *dup_iblock;
	struct ouichefs_inode_info *isrc, *idup;
	struct buffer_head *src_bh= NULL, *dup_bh = NULL;
	int i, j, ret = 0, len, last, count = 0;
	pr_info("-> deduplicate_file\n");
	
	/* Get inode_info of the two files */
	isrc = OUICHEFS_INODE(src);
	idup = OUICHEFS_INODE(dup);

	/* Read index blocks of the two inodes */
	src_bh = sb_bread(src->i_sb, isrc->index_block);
	if(!src_bh)
		return -EIO;
	dup_bh = sb_bread(dup->i_sb, idup->index_block);
	if(!dup_bh){
		ret = -EIO;
		goto BRELSE_SRC;
	}
	src_iblock = (struct ouichefs_file_index_block *)src_bh->b_data;
	dup_iblock = (struct ouichefs_file_index_block *)dup_bh->b_data;
	
	len = OUICHEFS_BLOCK_SIZE;
	last = src->i_size / OUICHEFS_BLOCK_SIZE; 

	/* Foreach reduntant block in dup inode, put it and reference origin one in src inode */
	for(i = 0; i< src->i_blocks - 1; i++){
		struct buffer_head *sbh = sb_bread(src->i_sb, src_iblock->blocks[i]);
		if(!sbh){
			ret = -EIO;
			goto BRELSE_DUP;
		}
		/* Amount of data to compare(check size of last block) */
		if(i == last) 
			len = src->i_size % OUICHEFS_BLOCK_SIZE; 
			
		for(j = 0; j < dup->i_blocks - 1; j++){
			uint32_t src_b = src_iblock->blocks[i];
			uint32_t dup_b = dup_iblock->blocks[j];
			if(src_b == dup_b) /* same block */
				continue;
			struct buffer_head *dbh = sb_bread(dup->i_sb, dup_b);
			if(!dbh){
				ret = -EIO;
				brelse(sbh);
				goto BRELSE_DUP;
			}
			/* If same content, put duplicated block, reference and increment link count of source block */
			if(memcmp(sbh->b_data, dbh->b_data, len) == 0){
				ouichefs_unlink_block(dup->i_sb, dup_b);
				dup_iblock->blocks[j] = src_b;
				ret = ouichefs_link_block(src->i_sb, src_b);
				if(ret < 0) /* must not happend */
					pr_warn("Error: link block failed !"); //goto BRELSE_DUP;
				count++;
				pr_info("-- src_block[%u] <-- dup_block[%u]\n", src_b, dup_b);
			}
			brelse(dbh);
		}
		brelse(sbh);
	}
	mark_buffer_dirty(dup_bh);
	
	ret = count;	
BRELSE_DUP:	
	brelse(dup_bh);
BRELSE_SRC:
	brelse(src_bh);
	return ret;
}
/*
 * Deduplicate all files that can share the data content. So, for each inode in
 * ouichefs_dup_block->iduplicated[] list, check if there is any other inode in
 * ouichefs_dup_block->isources[] list that can share the same block content.
 * Called in:
 * - super.c: ouichefs_put_super();
 * 
 * On success, the total number of block deduplicated is returned.
 */
int ouichefs_deduplicate(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_dup_block *dupblock;
	struct inode *isrc, *idup;
	struct buffer_head *bh;
	uint32_t sindex, dindex;
	uint32_t *isources, *iduplicated;
	int ret = 0, found = 0;
	
	bh = sb_bread(sb, sbi->index_dupblock);
	if(!bh)
		return -EIO;
	dupblock = (struct ouichefs_dup_block*)bh->b_data;
	isources = dupblock->isources;
	iduplicated = dupblock->iduplicated; 
	
	sindex = 1;
	dindex = 1;
	/* foreach inode in iduplicated[], check if it is a copy of another inode in isources[] */
	while(iduplicated[dindex] && dindex < (OUICHEFS_BLOCK_SIZE >> 3)){
		/* Get duplicate inode from index_dupblock->iduplicated[] */
		idup = ouichefs_iget(sb, iduplicated[dindex]);
		if (IS_ERR(idup)) {
			ret = PTR_ERR(idup); 
			goto END; 
		}
LOOP:		while(isources[sindex] && sindex < (OUICHEFS_BLOCK_SIZE >> 3)){
			if(isources[sindex] == iduplicated[dindex]) /* same inode */
				goto NEXT;			
			
			isrc = ouichefs_iget(sb, isources[sindex]);
			if (IS_ERR(isrc)) {
				ret = PTR_ERR(isrc); 
				goto PUTD;
			}
			found += deduplicate_file(isrc, idup);
			if(found < 0){
				ret = -1; 
				goto PUTS;
			}else if(found == idup->i_blocks - 1){ /* deduplication ends */ 
				iput(isrc);
				break;
			}
			iput(isrc);
NEXT:			sindex++;
		}
		/* If no duplicate block found in isources[], check in iduplicated[] */
		if(found == 0 && isources != iduplicated){
			isources = iduplicated;
			sindex = 1;
			goto LOOP;
		}else if(isources == iduplicated){ /* reset config */
			isources = dupblock->isources;
		}
		iput(idup);
		ret += found;
		found = 0;
		sindex = 1;
		dindex++;
	}
	memset(bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	
	pr_info("--> %d blocks deduplicated <--\n", ret);
	goto END;
PUTS:
	iput(isrc);
PUTD:
	iput(idup);
END:	
	brelse(bh);
	return ret;
}
/*
 * Add inode number on Read() into ouichefs_dup_block->isources[] list located 
 * in memory block (the number of the block is ouichefs_sb_info->index_dupblock).
 * Called in:
 * - file.c: ouichefs_readpage();
 * 
 * On success, 0 is returned.
 */
int dupblock_add_isrc(struct inode *inode)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	struct ouichefs_dup_block *dup_block;
	struct buffer_head *bh;
	uint32_t index;
	
	bh = sb_bread(inode->i_sb, sbi->index_dupblock);
	if(!bh)
		return -EIO;	
	dup_block = (struct ouichefs_dup_block*)bh->b_data;
	index  = dup_block->isources[0];
	
	/* If inode is not already in isrouces[] list and the list is not full */
	if(!test_and_set_bit(inode->i_ino, sbi->isrc_bitmap) && \
		++index < (OUICHEFS_BLOCK_SIZE >> 3)){
			
		dup_block->isources[index] = inode->i_ino;
		dup_block->isources[0] = index;
		mark_buffer_dirty(bh);
	}

	brelse(bh);
	return 0;
}
/*
 * Add inode number on Write() into ouichefs_dup_block->iduplicated[] list located
 * in memory block (the number of the block is ouichefs_sb_info->index_dupblock).
 * Called in:
 * - file.c: ouichefs_writepage();
 * 
 * On success, 0 is returned.
 */
int dupblock_add_idup(struct inode *inode)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	struct ouichefs_dup_block *dup_block;
	struct buffer_head *bh;
	uint32_t index;
	
	bh = sb_bread(inode->i_sb, sbi->index_dupblock);
	if(!bh)
		return -EIO;	
	dup_block = (struct ouichefs_dup_block*)bh->b_data;
	index  = dup_block->iduplicated[0];
	
	/* If inode is not already in iduplicated[] list and the list is not full */
	if(!test_and_set_bit(inode->i_ino, sbi->idup_bitmap) \
		&& ++index < (OUICHEFS_BLOCK_SIZE >> 3)){
		
		dup_block->iduplicated[index] = inode->i_ino;
		dup_block->iduplicated[0] = index;
		mark_buffer_dirty(bh);
	}
	
	brelse(bh);
	return 0;
}
