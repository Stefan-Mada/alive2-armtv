define i15 @f(ptr %0, i32 %z) {
  %2 = tail call signext i15 %0(i32 %z)
  ret i15 %2
}
