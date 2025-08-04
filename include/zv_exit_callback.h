/* Functions */
void zv_insert_exception_to_vm(void);
void zv_vm_exit_callback(struct zv_vm_exit_guest_register* guest_context);
void zv_vm_resume_fail_callback(u64 error);