define i1 @f(ptr %0, i32 %z) {
  %2 = tail call zeroext i1 %0(i32 %z)
  ret i1 %2
}
