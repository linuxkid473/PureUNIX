.section .text
.global context_switch
.type context_switch, @function
context_switch:
    mov 4(%esp), %eax
    mov 8(%esp), %edx
    pushfl
    pusha
    mov %esp, (%eax)
    mov %edx, %esp
    popa
    popfl
    ret
