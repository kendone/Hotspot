################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/jdk/windows/native/sun/security/provider/WinCAPISeedGenerator.c 

OBJS += \
./src/jdk/windows/native/sun/security/provider/WinCAPISeedGenerator.o 

C_DEPS += \
./src/jdk/windows/native/sun/security/provider/WinCAPISeedGenerator.d 


# Each subdirectory must supply rules for building sources it contributes
src/jdk/windows/native/sun/security/provider/%.o: ../src/jdk/windows/native/sun/security/provider/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: Cross GCC Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


