#include <linux/fs.h>
#include "babyfs.h"

const struct file_operations baby_file_operations = {
  .open = generic_file_open,
  .read_iter = generic_file_read_iter,
  .write_iter = generic_file_write_iter,
  .llseek		= generic_file_llseek,
  .fsync =generic_file_fsync,
};