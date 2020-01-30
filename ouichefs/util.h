#ifndef _UTIL_H
#define _UTIL_H


int dupblock_add_isrc(struct inode *inode);
int dupblock_add_idup(struct inode *inode);
int ouichefs_deduplicate(struct super_block *sb);
int deduplicate_file(struct inode *src, struct inode *dup);
int ouichefs_link_block(struct super_block *sb, uint32_t bno);
int ouichefs_unlink_block(struct super_block *sb, uint32_t bno);
int ouichefs_cow(struct file *file, uint32_t vb_index, uint32_t nr_towrite);

#endif /* _UTIL_H */
